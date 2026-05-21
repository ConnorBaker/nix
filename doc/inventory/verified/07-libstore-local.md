# Inventory — Shard 07: libstore local stores (verified)

Files in shard (24 total): the local-store family, gc machinery, sqlite wrapper, settings/globals, dummy and restricted stores, and the SQL schema files.

---

## File: src/libstore/local-store.cc

### Namespaces
- `nix` (single enclosing namespace)

### Classes / structs / enums
- `LocalStore::State::Stmts` — struct, nested member of `LocalStore::State`. Holds 15 `SQLiteStmt` precompiled handles in declaration order: `RegisterValidPath`, `UpdatePathInfo`, `AddReference`, `QueryPathInfo`, `QueryReferences`, `QueryReferrers`, `InvalidatePath`, `AddDerivationOutput`, `RegisterRealisedOutput`, `UpdateRealisedOutput`, `QueryValidDerivers`, `QueryDerivationOutputs`, `QueryRealisedOutput`, `QueryPathFromHashPart`, `QueryValidPaths`. The `RegisterRealisedOutput`/`UpdateRealisedOutput`/`QueryRealisedOutput` trio is only prepared when `Xp::CaDerivations` is enabled.
- `Free` — function-object struct local to `LocalStore::addToStoreFromDump` providing `void operator()(void *)` that calls `free()`; used as a `std::unique_ptr` deleter for a manually `realloc`'d byte buffer.

### Functions

#### Free / static functions
- `static RegisterStoreImplementation<LocalStore::Config> regLocalStore;` — file-local static registration object that registers `LocalStore` into the global store-implementation registry at startup.

#### Member functions on `LocalStoreConfig`
- `LocalStoreConfig::LocalStoreConfig(const std::filesystem::path & path, const Params & params)` — constructor that initialises `StoreConfig` (with `FilePathType::Native`) and the `LocalFSStoreConfig(path, params)` virtual base.
- `LocalStoreConfig::doc()` — returns the markdown documentation embedded from `local-store.md`.
- `LocalStoreConfig::getDefaultRequireSigs()` — returns global `settings.requireSigs`.
- `LocalStoreConfig::getRootsSocketPath()` — returns `<stateDir>/gc-roots-socket/socket`.
- `LocalStoreConfig::getReference()` — produces the `StoreReference` (back-compat: returns the bare `StoreReference::Local{}` variant when there are no query params, otherwise a `Specified{scheme="local"}`).
- `LocalStoreConfig::getReadOnly()` — returns `readOnly.get() || StoreConfig::getReadOnly()`.
- `LocalStoreConfig::anchor()` — empty override that pins the vtable in this TU.

#### Member functions on `LocalBuildStoreConfig`
- `LocalBuildStoreConfig::getLocalSettings()` — returns `settings.getLocalSettings()`.
- `LocalBuildStoreConfig::getBuildDir()` — resolves the build directory from `getLocalSettings().buildDir`, then this struct's own `buildDir`, then `<stateDir>/builds`.
- `LocalBuildStoreConfig::anchor()` — empty override.

#### Member functions on `LocalStore`
- `LocalStore::LocalStore(ref<const Config>)` — constructor; creates state subdirs (`realStoreDir`, `linksDir`, `profiles`, `temproots`, `dbDir`, `gcroots`, per-user dirs), optionally remounts store rw, sets up `reservedPath`, acquires shared big-lock, runs schema migrations (legacy in-line for schemas <8/<9/<10, then named via `upgradeDBSchema`), prepares all SQL statements (CA-derivation ones gated on `Xp::CaDerivations`).
- `LocalStore::~LocalStore()` — waits for any auto-GC future, closes/unlinks tempRoots file.
- `LocalStore::anchor()` — empty override.
- `LocalStore::Config::openStore()` — produces a new `LocalStore` via `make_ref<LocalStore>(ref{shared_from_this()})`.
- `LocalStore::openGCLock()` — opens/creates `<stateDir>/gc.lock`.
- `LocalStore::deleteStorePath(path, bytesFreed, isKnownPath)` — wraps `deletePath`; if `ignoreGcDeleteFailure`, logs warning instead of throwing.
- `LocalStore::getSchema()` — reads the `schema` file, returning 0 if absent.
- `LocalStore::openDB(state, create)` — opens SQLite DB with the appropriate `SQLiteOpenMode`; sets `pragma synchronous` (normal/off based on `fsyncMetadata`), journal mode (`wal`/`truncate`), `journal_size_limit`, `wal_autocheckpoint`; runs `schema.sql.gen.hh` if `create`.
- `LocalStore::upgradeDBSchema(state)` — creates `SchemaMigrations` table if missing; applies named idempotent migrations (currently `20251017-ca-derivations` from `ca-specific-schema.sql.gen.hh` when `Xp::CaDerivations` is on, and `20260309-drop-redundant-indexreferrer` which `drop index if exists IndexReferrer`).
- `LocalStore::makeStoreWritable()` — Linux-only; if running as root and `realStoreDir` is on a read-only mount, remounts it rw preserving relevant flags (`MS_NODEV`/`MS_NOSUID`/...).
- `LocalStore::registerDrvOutput(info, checkSigs)` — overload that, if `checkSigs == NoCheckSigs` or the realisation has a trusted signature, delegates to the no-`checkSigs` overload.
- `LocalStore::registerDrvOutput(info)` — under `retrySQLite`: if a compatible existing realisation exists, merges signatures via `UpdateRealisedOutput`; otherwise inserts a new `BuildTraceV3` row via `RegisterRealisedOutput`. Requires `Xp::CaDerivations`.
- `LocalStore::cacheDrvOutputMapping(state, deriver, outputName, output)` — under `retrySQLite`, inserts/replaces a row in `DerivationOutputs` via `AddDerivationOutput`.
- `LocalStore::addValidPath(state, info)` — INSERTs a `ValidPaths` row, runs `parsedDrv.checkInvariants` if the path is a derivation, caches its known derivation outputs via `cacheDrvOutputMapping`, updates the in-memory pathInfo cache, returns the new row id.
- `LocalStore::queryPathInfoUncached(path, callback)` — async wrapper around `queryPathInfoInternal` via `retrySQLite`.
- `LocalStore::queryPathInfoInternal(state, path)` — synchronous DB lookup returning `shared_ptr<ValidPathInfo>` populated with id, narHash, registrationTime, deriver, narSize, ultimate, sigs, ca, and references via `QueryReferences`.
- `LocalStore::updatePathInfo(state, info)` — UPDATEs `ValidPaths` row via `UpdatePathInfo`.
- `LocalStore::queryValidPathId(state, path)` — returns `id` column for path (throws `InvalidPath` if not present).
- `LocalStore::isValidPath_(state, path)` — boolean "is in DB?" without retry, using `QueryPathInfo`.
- `LocalStore::isValidPathUncached(path)` — wraps `isValidPath_` in `retrySQLite`.
- `LocalStore::queryValidPaths(paths, maybeSubstitute)` — set intersection with DB validity (via `isValidPath`).
- `LocalStore::queryAllValidPaths()` — selects every row's `path` via `QueryValidPaths`.
- `LocalStore::queryReferrers(state, path, referrers)` — DB-backed referrers query via `QueryReferrers`.
- `LocalStore::queryReferrers(path, referrers)` — `retrySQLite` wrapper.
- `LocalStore::queryValidDerivers(path)` — derivers via `QueryValidDerivers` (joins `DerivationOutputs` with `ValidPaths`).
- `LocalStore::queryStaticPartialDerivationOutputMap(path)` — output-name → optional store path via `QueryDerivationOutputs`.
- `LocalStore::queryStaticPartialDerivationOutput(path, outputName)` — single-output wrapper; throws when CA derivations is disabled and the output is missing, otherwise returns nullopt.
- `LocalStore::queryPathFromHashPart(hashPart)` — hash-prefix lookup via `QueryPathFromHashPart` (`select path >= ? limit 1`).
- `LocalStore::registerValidPath(info)` — single-path wrapper for `registerValidPaths`.
- `LocalStore::registerValidPaths(infos)` — under `retrySQLite` and `SQLiteTxn`: optionally `sync()` first; UPDATE existing rows or INSERT new ones; insert `Refs` rows for each reference; topological sort to detect cycles (rolls back on cycle). Pre-call `sync()` skipped on Windows.
- `LocalStore::invalidatePath(state, path)` — deletes a `ValidPaths` row via `InvalidatePath` and invalidates the in-memory cache; foreign-key cascades do the rest.
- `LocalStore::getPublicKeys()` — caches and returns `PublicKeys` (computed via `getDefaultPublicKeys()` on first access).
- `LocalStore::pathInfoIsUntrusted(info)` — `config->requireSigs && !info.checkSignatures(*this, getPublicKeys())`.
- `LocalStore::realisationIsUntrusted(realisation)` — analogous check for realisations.
- `LocalStore::addToStore(info, source, repair, checkSigs)` — addTempRoot, lock output path, restore NAR (with optional fsync), verify hash/size and the CA assertion (Flat/NixArchive via `HashModuloSink`, Git via `git::dumpHash`), trigger `autoGC`, canonicalise, optimise, optional recursive sync, register.
- `LocalStore::addToStoreFromDump(source, name, dumpMethod, hashMethod, hashAlgo, references, repair)` — buffered add-from-dump path producing a CA store path; restores into a temp dir if buffer fills, finally moves into place at the dest store path.
- `LocalStore::createTempDirInStore()` — creates a directory under `realStoreDir` named `tmp-...`, opens it, write-locks it, returns `(path, fd)` (loops until both succeed even if a concurrent GC raced).
- `LocalStore::invalidatePathChecked(path)` — under `retrySQLite` + `SQLiteTxn`: invalidates after asserting no non-self referrers (else throws `PathInUse`).
- `LocalStore::verifyStore(checkContents, repair)` — top-level integrity check; takes the GC lock; calls `verifyAllValidPaths`; optionally hashes `.links/` files and each path's content; updates missing `narHash`/`narSize`; can repair.
- `LocalStore::verifyAllValidPaths(repair)` — initial sweep: builds set of paths in store dir, then for every path returned by `queryAllValidPaths` calls `verifyPath`; returns `VerificationResult`.
- `LocalStore::verifyPath(path, existsInStoreDir, done, validPaths, repair, errors)` — recursive per-path verification: if missing on disk, recurses into referrers and either invalidates or errors; otherwise inserts into `validPaths`.
- `LocalStore::getProtocol()` — returns `WorkerProto::latest.number.toWire()`.
- `LocalStore::isTrustedClient()` — always returns `Trusted`.
- `LocalStore::vacuumDB()` — runs `vacuum` via `db.exec`.
- `LocalStore::addSignatures(storePath, sigs)` — under `retrySQLite` + `SQLiteTxn`: merges sigs into the path's `ValidPathInfo`, calls `updatePathInfo`.
- `LocalStore::queryRealisationCore_(state, id)` — internal lookup returning `(realisationDbId, UnkeyedRealisation)` from `QueryRealisedOutput`.
- `LocalStore::queryRealisation_(state, id)` — wrapper returning just the `UnkeyedRealisation`.
- `LocalStore::queryRealisationUncached(id, callback)` — async wrapper around `queryRealisation_` via `retrySQLite`.
- `LocalStore::addBuildLog(drvPath, log)` — bzip2-compresses log, atomic-rename into `<logDir>/drvs/<aa>/<rest>.bz2`.
- `LocalStore::getVersion()` — returns `nixVersion`.

#### Member functions on `GcStore`
- `GcStore::anchor()` — empty override defined in this TU as a vtable-pinning convenience for the otherwise abstract mix-in.

Note: `LocalStore::addIndirectRoot`, `createTempRootsFile`, `addTempRoot`, `findTempRoots`, `findRoots(path,type,roots)`, `findRootsNoTemp`, `findRoots(censor)`, `findRuntimeRoots`, `collectGarbage`, `autoGC` live in `gc.cc` (same shard) — see that file. The `optimiseStore`/`optimisePath`/`loadInodeHash`/`readDirectoryIgnoringInodes`/`optimisePath_` group lives in `optimise-store.cc` (different shard).

### Type aliases
None new.

### Macros / globals
- File-local static `regLocalStore` (above) is the only file-scope global.
- Uses `NIX_WHEN_SUPPORT_ACLS` macro from a different shard.

---

## File: src/libstore/include/nix/store/local-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `OptimiseStats` — struct: `unsigned long filesLinked = 0`, `uint64_t bytesFreed = 0`. Reporting struct for store optimisation.
- `LocalSettings` — forward declaration only.
- `LocalBuildStoreConfig` — struct (`virtual LocalFSStoreConfig`); private `void anchor() override`; private `Setting<std::optional<AbsolutePath>> buildDir` (name `"build-dir"`); public `getLocalSettings()` and `getBuildDir()`.
- `LocalStoreConfig` — struct, `std::enable_shared_from_this<LocalStoreConfig>`, `virtual LocalFSStoreConfig`, `virtual LocalBuildStoreConfig`. Two constructors: `(Params)` (header-defined, sets `FilePathType::Native`) and `(path, Params)` (defined in `local-store.cc`). Private `anchor()`, `getDefaultRequireSigs()`. Public `Setting<bool>`s: `requireSigs`, `readOnly`, `ignoreGcDeleteFailure` (gated on `Xp::LocalOverlayStore`), `useRootsDaemon` (gated on `Xp::LocalOverlayStore`). Public methods: `getReadOnly()`, `getRootsSocketPath()`, static `name()`, static `uriSchemes()`, static `doc()`, `openStore()`, `getReference()`.
- `LocalStore` — class, `public virtual IndirectRootStore, public virtual GcStore`. Private leading `void anchor() override`. Members detailed below.
- `LocalStore::State` — nested struct: `SQLite db`; forward declaration `struct Stmts`; `std::unique_ptr<Stmts> stmts`; `std::chrono::time_point<std::chrono::steady_clock> lastGCCheck`; `bool gcRunning = false`; `std::shared_future<void> gcFuture`; `uint64_t availAfterGC = numeric_limits<uint64_t>::max()`; `std::unique_ptr<PublicKeys> publicKeys`.
- `LocalStore::VerificationResult` — protected nested struct: `bool errors`, `StorePathSet validPaths`.

#### `LocalStore` data members
- Private: `AutoCloseFD globalLock`, `ref<Sync<State>> _state`.
- Public path members (all `const std::filesystem::path`): `dbDir`, `linksDir`, `reservedPath`, `schemaPath`, `tempRootsDir`, `fnTempRoots`.
- Private: `Sync<AutoCloseFD> _fdTempRoots`, `_fdGCLock`, `_fdRootsSocket`.
- Public: `ref<const LocalStoreConfig> config`, `PathSet locksHeld`.
- Friends: `struct PathSubstitutionGoal`, `struct DerivationGoal`, `class DerivationBuilderImpl` (only for `createTempDirInStore`).

#### `LocalStore` declared methods
- Public abstract-`Store` overrides: `isValidPathUncached`, `queryValidPaths`, `queryAllValidPaths`, `queryPathInfoUncached`, `queryReferrers(path, referrers)`, `queryValidDerivers`, `queryStaticPartialDerivationOutputMap`, `queryStaticPartialDerivationOutput`, `queryPathFromHashPart`, `pathInfoIsUntrusted`, `realisationIsUntrusted`, `addToStore`, `addToStoreFromDump`, `addTempRoot`.
- Private: `void createTempRootsFile()`.
- Public override of `IndirectRootStore::addIndirectRoot`.
- Private: `findTempRoots(roots, censor)`, `openGCLock()`.
- Public override of `GcStore::findRoots(censor)`, `GcStore::collectGarbage(options, results)`.
- Public new virtuals (overridable): `queryGCReferrers(path, referrers)` (inline default delegates to `queryReferrers`), `deleteStorePath(path, bytesFreed, isKnownPath)`.
- Public: `optimiseStore(stats)` (declaration; defined in optimise-store.cc), `optimiseStore()` override, `optimisePath(path, repair)`, `verifyStore(checkContents, repair)` override.
- Protected virtual `verifyAllValidPaths(repair)`.
- Public: `registerValidPath(info)`; virtual `registerValidPaths(infos)`; `getProtocol()` override; `isTrustedClient()` override; `vacuumDB()`; `addSignatures(storePath, sigs)` override; `autoGC(sync)`; overrides `registerDrvOutput(info)` and `registerDrvOutput(info, checkSigs)`; `cacheDrvOutputMapping`. Public: `queryRealisation_`, `queryRealisationCore_`, `queryRealisationUncached` override. Public `getVersion()` override.
- Protected: `verifyPath`.
- Private: `getSchema`, `openDB`, `upgradeDBSchema`, `makeStoreWritable`, `queryValidPathId`, `addValidPath`, `invalidatePath`, `invalidatePathChecked`, `queryPathInfoInternal`, `updatePathInfo`, `findRoots(path,type,roots)`, `findRootsNoTemp`, `findRuntimeRoots`, `createTempDirInStore`, `loadInodeHash`, `readDirectoryIgnoringInodes`, `optimisePath_`, `isValidPath_`, `queryReferrers(state, path, referrers)`, `addBuildLog` override, `getPublicKeys()`.
- Private nested type `typedef boost::unordered_flat_set<ino_t> InodeHash`.

### Type aliases
- `LocalStore::Config = LocalStoreConfig` (`using`).
- Private nested `typedef boost::unordered_flat_set<ino_t> InodeHash`.

### Macros / globals
- `const int nixSchemaVersion = 10` — current expected schema version.

---

## File: src/libstore/local-fs-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `LocalStoreAccessor` — file-local struct, `: SourceAccessor`. Members: `ref<SourceAccessor> accessor` (an `FSSourceAccessor` rooted at `realStoreDir`), `ref<LocalFSStore> store`, `bool requireValidPath`. Constructor `(ref<LocalFSStore>, bool)`. Helper `requireStoreObject(path)` (asserts the path corresponds to a valid store path when `requireValidPath`). Overrides: `maybeLstat`, `lstat`, `readDirectory` (two overloads — one returning `DirEntries`, one taking a callback), `readFile`, `readLink`, `showPath`, `getPhysicalPath`, `getFingerprint`, `getLastModified`, `pathExists`.

### Functions
- `LocalFSStoreConfig::anchor()` — empty override.
- `LocalFSStoreConfig::LocalFSStoreConfig(rootDir, params)` — constructor; manually rebuilds the `rootDir` setting via `makeRootDirSetting` (see comment about virtual base init order forcing the duplication of the setting's normalisation logic).
- `LocalFSStore::anchor()` — empty override.
- `LocalFSStore::LocalFSStore(const Config &)` — constructor; delegates `Store{*this}` and stores a config reference.
- `LocalFSStore::getFSAccessor(bool requireValidPath)` override — returns a fresh `LocalStoreAccessor` covering the whole store dir.
- `LocalFSStore::getFSAccessor(const StorePath &, bool requireValidPath)` override — single-path accessor; returns nullptr if the path is invalid (or absent on disk when `requireValidPath` is false).
- `LocalFSStore::getBuildLogExact(path)` override — searches `<logDir>/drvs/<aa>/<rest>` and `<logDir>/drvs/<full>` (in a `for j=0;j<2;j++` loop) along with `.bz2` variants, returning decompressed-or-plain text.

### Type aliases
None new.

### Macros / globals
- `const std::filesystem::path LocalFSStore::drvsLogDir = "drvs"` — definition of the static drv log directory name.

---

## File: src/libstore/include/nix/store/local-fs-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `LocalFSStoreConfig` — struct (`virtual StoreConfig`). Private `anchor()`. Private static helper `makeRootDirSetting(self, defaultValue)` returning a `Setting<std::optional<AbsolutePath>>` with name `"root"`. Public single-arg constructor `(Params)` (initialises `StoreConfig` with `FilePathType::Native`); separate-`(path, Params)` constructor declared, defined in `local-fs-store.cc`. Public `Setting`s: `rootDir = makeRootDirSetting(*this, std::nullopt)`, `stateDir`, `logDir`, `realStoreDir`. Inline overrides: `getStateDir()` returns `stateDir.get()`, `getLogDir()` returns `logDir.get()`.
- `LocalFSStore` — struct (with `alignas(8)` workaround for ASAN i686-linux failures), `virtual Store`, `virtual GcStore`, `virtual LogStore`. Private `anchor()`. Public alias `Config = LocalFSStoreConfig`. Public members: `const Config & config`, `inline static std::string operationName = "Local Filesystem Store"`, `static const std::filesystem::path drvsLogDir`. Constructor `(const Config &)`. Overrides: `getFSAccessor(bool)`, `getFSAccessor(const StorePath &, bool)`. Pure virtual `addPermRoot(storePath, gcRoot)`. Virtual `getRealStoreDir()` (returns `config.realStoreDir`). Non-virtual inline `toRealPath(storePath)` (returns `getRealStoreDir() / storePath.to_string()`). Override `getBuildLogExact(path)`.

### Type aliases
- `LocalFSStore::Config = LocalFSStoreConfig` (`using`).

### Macros / globals
None new.

---

## File: src/libstore/local-overlay-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
None new (only impls of `LocalOverlayStore`/`LocalOverlayStoreConfig`).

### Functions
- `LocalOverlayStoreConfig::anchor()` — empty override.
- `LocalOverlayStore::anchor()` — empty override.
- `LocalOverlayStoreConfig::doc()` — embeds `local-overlay-store.md`.
- `LocalOverlayStoreConfig::openStore()` override — `make_ref<LocalOverlayStore>(...)`, dynamic-casting `shared_from_this()` to `const LocalOverlayStoreConfig`.
- `LocalOverlayStoreConfig::getReference()` override — returns `Specified{scheme="local-overlay"}`.
- `LocalOverlayStoreConfig::toUpperPath(storePath)` — returns `upperLayer.get() / storePath.to_string()`.
- `LocalOverlayStore::LocalOverlayStore(ref<const Config>)` — constructor; opens `lowerStore` from `lowerStoreUri` (must be `LocalFSStore`), throws if `upper-layer` was not overridden, optionally checks `/proc/self/mounts` to verify lower/upper dirs match the configured ones.
- `LocalOverlayStore::registerDrvOutput(info)` override — first calls `lowerStore->queryRealisation`; if found, registers the lower realisation locally too; then registers the new info via `LocalStore::registerDrvOutput`.
- `LocalOverlayStore::queryPathInfoUncached(path, callback)` override — calls `LocalStore::queryPathInfoUncached`; on null result, falls through to `lowerStore->queryPathInfo` via a chained continuation.
- `LocalOverlayStore::queryRealisationUncached(drvOutput, callback)` override — same pattern: try upper, on miss call `lowerStore->queryRealisation`.
- `LocalOverlayStore::isValidPathUncached(path)` override — try upper; on lower hit, recurse into closure to ensure it's all valid in lower, then `LocalStore::registerValidPath` to materialise upper-DB metadata.
- `LocalOverlayStore::queryReferrers(path, referrers)` override — union of upper (`LocalStore::queryReferrers`) and lower-store referrers.
- `LocalOverlayStore::queryGCReferrers(path, referrers)` override — only upper-store referrers (GC operates only on upper layer).
- `LocalOverlayStore::queryValidDerivers(path)` override — union of upper and lower derivers.
- `LocalOverlayStore::queryPathFromHashPart(hashPart)` override — upper first; else lower.
- `LocalOverlayStore::registerValidPaths(infos)` override — first copies up any not-in-upper predecessors found in lower (via `lowerStore->queryValidPaths`/`queryPathInfo`), then `LocalStore::registerValidPaths(infos)`.
- `LocalOverlayStore::collectGarbage(options, results)` override — calls `LocalStore::collectGarbage`, then `remountIfNecessary`.
- `LocalOverlayStore::deleteStorePath(path, bytesFreed, isKnownPath)` override — if path lives in upper and is also valid in lower, deletes via `deletePath(upperPath)` and sets `_remountRequired`; otherwise delegates to `LocalStore::deleteStorePath`. Warns and returns early if `path` is not directly under `realStoreDir`.
- `LocalOverlayStore::optimiseStore()` override — for each upper-DB-known path that also exists in lower, delete the upper copy via `deleteStorePath` (deduplicating); ends with `remountIfNecessary`.
- `LocalOverlayStore::verifyAllValidPaths(repair)` override — like `LocalStore::verifyAllValidPaths` but only walks upper-DB paths and uses a looser existence predicate (`pathExists(realStoreDir/path)`).
- `LocalOverlayStore::remountIfNecessary()` — runs `remountHook` (or warns if unset) when `_remountRequired` is set; clears the flag after.
- `static RegisterStoreImplementation<LocalOverlayStore::Config> regLocalOverlayStore;` — registration.

### Type aliases
None new.

### Macros / globals
None new.

---

## File: src/libstore/include/nix/store/local-overlay-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `LocalOverlayStoreConfig` — struct, `virtual LocalStoreConfig`. Private `anchor()`. Two constructors: `(StringMap params)` (delegates to the `(path, params)` form with empty path) and `(path, params)` (initialises `StoreConfig`, `LocalFSStoreConfig`, `LocalStoreConfig`). Public `Setting<std::string> lowerStoreUri`, `const Setting<AbsolutePath> upperLayer`, `Setting<bool> checkMount` (default true), `const Setting<std::optional<AbsolutePath>> remountHook`. Static methods `name()` (returns `"Experimental Local Overlay Store"`), `experimentalFeature()` (returns `ExperimentalFeature::LocalOverlayStore`), `uriSchemes()` (returns `{"local-overlay"}`), `doc()`. Overrides `openStore()`, `getReference()`. Protected `toUpperPath(path)`. Friend `struct LocalOverlayStore`.
- `LocalOverlayStore` — struct, `virtual LocalStore`. Public alias `Config = LocalOverlayStoreConfig`. Public `ref<const Config> config`. Constructor `(ref<const Config>)`. Private `anchor()`. Private `ref<LocalFSStore> lowerStore`. Private overrides: `registerDrvOutput`, `queryPathInfoUncached`, `isValidPathUncached`, `queryReferrers`, `queryValidDerivers`, `queryPathFromHashPart`, `registerValidPaths`, `queryRealisationUncached`, `collectGarbage`, `deleteStorePath`, `optimiseStore`, `verifyAllValidPaths`, `queryGCReferrers`. Private helper `remountIfNecessary()`. Private `std::atomic_bool _remountRequired = false`.

### Type aliases
- `LocalOverlayStore::Config = LocalOverlayStoreConfig` (`using`).

### Macros / globals
None new.

---

## File: src/libstore/local-gc.cc

### Namespaces
- `nix`

### Classes / structs / enums
None new.

### Functions
- `static void readProcLink(file, roots)` — reads a symlink target into `UncheckedRoots`, suppressing `no_such_file_or_directory`/`permission_denied`/`no_such_process` errors.
- `static std::string quoteRegexChars(raw)` — escapes regex metacharacters via Boost regex.
- `static void readFileRoots(path, roots)` — Linux-only helper that reads a file (e.g. `/proc/sys/kernel/modprobe`) and treats the contents as a root path target.
- `Roots findRuntimeRootsUnchecked(const StoreDirConfig & config)` — exported. Iterates `/proc/<pid>/{exe,cwd,fd,maps,environ}` collecting candidate store-path targets; if not Linux (and `_NIX_TEST_NO_LSOF != "1"`), shells out to `lsof -nwF n`; on Linux, additionally reads `/proc/sys/kernel/{modprobe,fbsplash,poweroff_cmd}`; finally translates and filters to recognised valid store paths.

### Type aliases
- File-local typedef `UncheckedRoots = boost::unordered_flat_map<std::string, boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>>, StringViewHash, std::equal_to<>>` — scratch storage; the comment notes the `std::string` key is forced because `std::filesystem::path` hashing is unavailable under macOS's libc++.

### Macros / globals
None new (uses `LSOF`, `OS_STR` macros from `store-config-private.hh`).

---

## File: src/libstore/include/nix/store/local-gc.hh

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `Roots findRuntimeRootsUnchecked(const StoreDirConfig & config)` — declared (defined in `local-gc.cc`).

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libstore/gc.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `GCLimitReached` — empty file-local struct used as a control-flow exception when `maxFreed` bytes have been deleted during `collectGarbage`.
- `Shared` — file-local struct nested inside `LocalStore::collectGarbage`. Fields: `boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>> tempRoots` (stores hash parts so suffixes like `.lock`/`.chroot`/`.check` are uniformly ignored) and `std::optional<std::string> pending`. Carries shared state between the GC main thread and the roots-server thread.

### Functions

#### Free / static functions
- `static std::string gcSocketPath = "gc-socket/socket"` — relative path under stateDir of the GC server's Unix-domain socket.
- `static std::string gcRootsDir = "gcroots"` — relative path under stateDir of the GC roots tree.
- `static std::string censored = "{censored}"` — placeholder used when censoring root sources.
- `static Roots requestRuntimeRoots(const LocalStoreConfig & config, const std::filesystem::path & socketPath)` — connects to the externally-run roots daemon, reads NUL-terminated store paths until empty line, and returns them as a `Roots` map (all entries marked `censored`).

#### Member functions on `LocalStore`
- `LocalStore::addIndirectRoot(path)` — implements `IndirectRootStore::addIndirectRoot`; symlinks `<gcroots>/auto/<sha1(path)>` → `path` via `makeSymlink`.
- `LocalStore::createTempRootsFile()` — creates and write-locks `<tempRootsDir>/<pid>`, retrying past concurrent GC reaping (loop reattempts when the file size becomes 0 mid-acquisition).
- `LocalStore::addTempRoot(path)` — registers a temp root via the per-process temp-roots file; if the global GC lock is held in shared mode by a running collector, opens the GC client socket and forwards the path; reconnects on `connection_refused`/`no_such_file_or_directory`/`broken_pipe`/`connection_reset`/EOF. No-op if `readOnly`.
- `LocalStore::findTempRoots(tempRoots, censor)` — reads each per-pid file in `tempRootsDir` (skipping hidden files); files whose lock can be acquired exclusively are reaped as stale and `'d'` written before unlinking; surviving files yield NUL-separated store paths emplaced into `tempRoots`.
- `LocalStore::findRoots(path, type, roots)` — recursive walker for a `gcroots` tree branch; understands directories, in-store symlinks, indirect-root symlinks (under `auto/`, with stale removal when the indirected target has disappeared), and regular files whose basename is itself a store path.
- `LocalStore::findRootsNoTemp(roots, censor)` — runs `findRoots` over `<stateDir>/{gcroots,profiles}`, then `findRuntimeRoots`.
- `LocalStore::findRoots(censor)` — public entry: `findRootsNoTemp` + `findTempRoots`.
- `LocalStore::findRuntimeRoots(roots, censor)` — uses `requestRuntimeRoots` if `useRootsDaemon` (requires `Xp::LocalOverlayStore`), else `findRuntimeRootsUnchecked`; filters out paths invalid in this store.
- `LocalStore::collectGarbage(options, results)` — main GC entry: spawns a non-blocking Unix-domain socket server on `<stateDir>/gc-socket/socket` (in a `std::thread`) to receive new temp roots from clients while collection is in progress; visits each candidate's referrer/derivation closure (`maybeDeleteReferrersClosure`); honours `keepOutputs`/`keepDerivations`; deletes orphan files in store dir; finally trims unused `linksDir` files (link count 1) and reports approximate hard-link savings. Throws/catches `GCLimitReached` to honour `maxFreed`. Windows: `_WIN32` branch throws `UnimplementedError` for the external GC client. The local helper lambda `deleteFromStore` enforces a write-lock on `tmp-` directories before deletion.
- `LocalStore::autoGC(sync)` — guarded on `HAVE_STATVFS`. Checks free space (via `statvfs` or `_NIX_TEST_FREE_SPACE_FILE`) at most once per `gcSettings.minFreeCheckInterval`; if below `minFree`/`maxFree` and not just barely above `availAfterGC * 0.97`, launches a detached async GC thread to free `gcSettings.maxFree - avail` bytes; optionally waits on the resulting future.

### Type aliases
None new.

### Macros / globals
- The three `static std::string`s above: `gcSocketPath`, `gcRootsDir`, `censored`.

---

## File: src/libstore/include/nix/store/gc-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `GCAction` — enum class: `gcReturnLive`, `gcReturnDead`, `gcDeleteDead`, `gcDeleteSpecific`.
- `GCOptions` — struct. `using GCAction = nix::GCAction;` and `using enum GCAction;`. Nested empty `WholeStore` and `SpecificPaths { StorePathSet paths; bool deleteReferrers = false; }`. Fields: `GCAction action{gcDeleteDead}`, `bool ignoreLiveness{false}`, `using GCPaths = std::variant<WholeStore, SpecificPaths>`, `GCPaths pathsToDelete`, `uint64_t maxFreed{numeric_limits<uint64_t>::max()}`.
- `GCResults` — struct: `StringSet paths`, `uint64_t bytesFreed = 0`.
- `GcStore` — struct, `public virtual Store`. Mix-in. Private `void anchor() override`. Public `inline static std::string operationName = "Garbage collection"`. Pure virtual `Roots findRoots(bool censor)`, `void collectGarbage(const GCOptions & options, GCResults & results)`.

### Type aliases
- `typedef boost::unordered_flat_map<StorePath, boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>>, std::hash<StorePath>> Roots` — root map shared across the GC family of files.
- `GCOptions::GCAction = nix::GCAction` (`using`).
- `GCOptions::GCPaths = std::variant<WholeStore, SpecificPaths>` (`using`).

### Macros / globals
None.

---

## File: src/libstore/indirect-root-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
None new.

### Functions
- `IndirectRootStore::anchor()` — empty override.
- `IndirectRootStore::makeSymlink(link, target)` — creates parent directories, writes the symlink to `<link>.tmp-<pid>-<rand>`, then atomically renames it over `link`.
- `IndirectRootStore::addPermRoot(storePath, _gcRoot)` — final implementation of `LocalFSStore::addPermRoot`: rejects roots inside the store, calls `addTempRoot(storePath)`, refuses to clobber an existing non-store-targeting symlink at `gcRoot`, writes the symlink via `makeSymlink`, registers via `addIndirectRoot`, and returns the canonicalised root path.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libstore/include/nix/store/indirect-root-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `IndirectRootStore` — struct, `public virtual LocalFSStore`. Private `void anchor() override`. Public `inline static std::string operationName = "Indirect GC roots registration"`. Public `final override` of `addPermRoot(storePath, gcRoot)`. Public pure virtual `addIndirectRoot(path)`. Protected `makeSymlink(link, target)`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libstore/sqlite.cc

### Namespaces
- `nix`

### Classes / structs / enums
None new.

### Functions
- `SQLiteError::SQLiteError(path, errMsg, errNo, extendedErrNo, offset, hf)` — constructor; builds `err.msg` with `sqlite3_errstr` plus offset.
- `[[noreturn]] static void SQLiteError::throw_(sqlite3 * db, HintFmt && hf)` — classifies as `SQLiteBusy` for `SQLITE_BUSY`/`SQLITE_PROTOCOL`, otherwise plain `SQLiteError`.
- `static void traceSQL(void * x, const char * sql)` — `notice(...)`-based callback registered via `sqlite3_trace` when `NIX_DEBUG_SQLITE_TRACES=1`.
- `SQLite::SQLite(const std::filesystem::path & path, Settings && settings)` — constructor; on Linux runs a ZFS workaround that `fdatasync`s the `*-shm` file when on a ZFS filesystem; selects VFS (`unix-dotfile` if not WAL); opens via `sqlite3_open_v2` with a `file:` URI carrying `?immutable=...`; sets a 1-hour busy timeout; optionally registers `traceSQL`; runs `pragma foreign_keys = 1`.
- `SQLite::~SQLite()` — closes DB via `sqlite3_close`; swallows exceptions in dtor.
- `SQLite::isCache()` — runs `pragma synchronous = off` and `pragma main.journal_mode = wal`.
- `SQLite::exec(stmt)` — wraps `sqlite3_exec` in `retrySQLite`.
- `SQLite::getLastInsertedRowId()` — wraps `sqlite3_last_insert_rowid`.
- `SQLiteStmt::create(db, sql)` — prepares the statement (asserts not already prepared).
- `SQLiteStmt::~SQLiteStmt()` — finalises the statement (swallows exceptions in dtor).
- `SQLiteStmt::Use::Use(SQLiteStmt &)` — resets statement.
- `SQLiteStmt::Use::~Use()` — resets statement.
- `SQLiteStmt::Use::operator()(string_view, notNull)` — bind text or null.
- `SQLiteStmt::Use::operator()(const unsigned char *, size_t, notNull)` — bind blob or null.
- `SQLiteStmt::Use::operator()(int64_t, notNull)` — bind int or null.
- `SQLiteStmt::Use::bind()` — bind null at next position.
- `SQLiteStmt::Use::step()` — wraps `sqlite3_step`.
- `SQLiteStmt::Use::exec()` — step expecting `SQLITE_DONE` (asserts not `SQLITE_ROW`).
- `SQLiteStmt::Use::next()` — step expecting `SQLITE_DONE` or `SQLITE_ROW`; returns whether a row is available.
- `SQLiteStmt::Use::getStr(col)` / `getInt(col)` / `isNull(col)` — column accessors.
- `SQLiteTxn::SQLiteTxn(db)` — runs `begin;`, sets `active = true`.
- `SQLiteTxn::commit()` — runs `commit;`, clears `active`.
- `SQLiteTxn::~SQLiteTxn()` — runs `rollback;` if still active (swallows exceptions in dtor).
- `void handleSQLiteBusy(const SQLiteBusy & e, time_t & nextWarning)` — sleeps a small random interval and warns periodically.

### Type aliases
None new.

### Macros / globals
None new.

---

## File: src/libstore/include/nix/store/sqlite.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `SQLiteOpenMode` — enum class: `Normal`, `NoCreate`, `Immutable`.
- `SQLiteSettings` — struct: `SQLiteOpenMode mode = SQLiteOpenMode::Normal`, `bool useWAL`.
- `SQLite` — struct. RAII handle. Member `sqlite3 * db = 0`. Default ctor. Alias `Settings = SQLiteSettings`. `SQLite(path, Settings &&)`; deleted copy ctor and copy assignment; move-assignment (`noexcept`); destructor; conversion `operator sqlite3 *()`; `isCache()`; `exec(stmt)`; `getLastInsertedRowId()`. (Note: no move constructor explicitly declared.)
- `SQLiteStmt` — struct. RAII prepared statement. Members `sqlite3 * db = 0`, `sqlite3_stmt * stmt = 0`, `std::string sql`. Default ctor; `(db, sql)` ctor that calls `create`. Methods `create(db, s)`, dtor, conversion `operator sqlite3_stmt *()`, `Use use()`. Nested class `Use` with `friend struct SQLiteStmt`: private `SQLiteStmt & stmt`, `unsigned int curArg = 1`, private constructor; public destructor and `operator()` overloads for `string_view`, `const unsigned char *`/`size_t`, `int64_t`; `bind()`, `step()`, `exec()`, `next()`, `getStr(col)`, `getInt(col)`, `isNull(col)`.
- `SQLiteTxn` — struct: `bool active = false`, `sqlite3 * db`; constructor; `commit()`; destructor (rolls back when still active).
- `SQLiteError` — struct, `: CloneableError<SQLiteError, Error>`. Members `std::string path`, `std::string errMsg`, `int errNo`, `extendedErrNo`, `offset`. Public constructor `(path, errMsg, errNo, extendedErrNo, offset, HintFmt &&)`. Public template `[[noreturn]] static void throw_(sqlite3 * db, const std::string & fs, const Args &... args)` forwarding to the `HintFmt` overload. Protected templated constructor and protected `[[noreturn]] static void throw_(sqlite3 * db, HintFmt && hf)`.

### Functions
- `void handleSQLiteBusy(const SQLiteBusy & e, time_t & nextWarning)` — declared.
- `template<typename T, typename F> T retrySQLite(const F & fun)` — header-only retry loop that catches `SQLiteBusy` and calls `handleSQLiteBusy`.

### Type aliases
- `SQLite::Settings = SQLiteSettings` (`using`).

### Macros / globals
- `MakeError(SQLiteBusy, SQLiteError);` — declares the derived exception via the macro from `util/error.hh`.

---

## File: src/libstore/include/nix/store/local-settings.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `SandboxMode` — typedef enum `{ smEnabled, smRelaxed, smDisabled }`.
- `BaseSetting<PathsInChroot>::trait` — full template specialisation: `static constexpr bool appendable = true`. (The corresponding `BaseSetting<SandboxMode>::trait` specialisation lives in `globals.cc`, not here.)
- `GCSettings` — struct, `public virtual Config`. `Setting<off_t> reservedSize` (default 8MB), `Setting<bool> keepOutputs`, `Setting<bool> keepDerivations` (default true), `Setting<uint64_t> minFree` (default 0), `Setting<uint64_t> maxFree` (default `int64_t max`, with the comment that this is intentionally not `uint64_t max` for Nix-language JSON compatibility), `Setting<uint64_t> minFreeCheckInterval` (default 5 seconds).
- `AutoAllocateUidSettings` — struct, `public virtual Config`. `Setting<uint32_t> startId` (Linux default `0x34000000`, else `56930`), `Setting<uint32_t> uidCount` (Linux: `maxIdsPerBuild * 128`, else `128`).
- `LocalSettings` — struct, `public virtual Config, public GCSettings, public AutoAllocateUidSettings`. Inline methods: `getGCSettings()` (mutable + const) returning `*this`, `getAutoAllocateUidSettings() const` returning `this` if `autoAllocateUids` else `nullptr`, `getDiffHook() const` returning `&diffHook.get()` if `runDiffHook` else `nullptr`, `findExternalDerivationBuilderIfSupported(drv)` (definition in `globals.cc`).
- `LocalSettings::ExternalBuilders` — `using ExternalBuilders = std::vector<ExternalBuilder>`.

#### `LocalSettings` `Setting<T>` fields (exhaustive, in declaration order)
- `Setting<unsigned int> buildCores` (name `"cores"`, alias `build-cores`).
- `Setting<bool> fsyncMetadata` (default true).
- `Setting<bool> fsyncStorePaths`.
- `Setting<bool> syncBeforeRegistering` (non-Windows only).
- `Setting<bool> autoOptimiseStore`.
- `Setting<size_t> narBufferSize` (default 32 MB).
- `Setting<bool> allowSymlinkedStore`.
- `Setting<std::string> buildUsersGroup` (the `documentDefault=false` flag is set at the 6th argument).
- `Setting<bool> autoAllocateUids` (gated on `Xp::AutoAllocateUids`).
- `Setting<bool> useCgroups` (Linux only).
- `Setting<bool> impersonateLinux26` (alias `build-impersonate-linux-26`).
- `Setting<SandboxMode> sandboxMode` (default `smEnabled` on Linux/FreeBSD, `smDisabled` elsewhere; aliases `build-use-chroot`, `build-use-sandbox`).
- `Setting<PathsInChroot> sandboxPaths` (aliases `build-chroot-dirs`, `build-sandbox-paths`).
- `Setting<bool> sandboxFallback` (default true).
- `Setting<bool> requireDropSupplementaryGroups` (non-Windows; default `isRootUser()`).
- `Setting<std::string> sandboxShmSize` (Linux only; default `"50%"`).
- `Setting<AbsolutePath> sandboxBuildDir` (Linux/FreeBSD only; default `/build`).
- `Setting<std::optional<AbsolutePath>> buildDir`.
- `Setting<std::set<std::filesystem::path>> allowedImpureHostPrefixes` (name `allowed-impure-host-deps`).
- `Setting<bool> darwinLogSandboxViolations` (Darwin only).
- `Setting<bool> runDiffHook`.
- private `Setting<std::optional<AbsolutePath>> diffHook` — accessed only via `getDiffHook()`.
- `Setting<std::string> preBuildHook`.
- `Setting<bool> filterSyscalls` (Linux only; default true).
- `Setting<bool> allowNewPrivileges` (Linux only).
- `Setting<StringSet> ignoredAcls` (only when `NIX_SUPPORT_ACL`).
- `Setting<StringMap> impureEnv` (gated on `Xp::ConfigurableImpureEnv`).
- `Setting<Strings> hashedMirrors`.
- `Setting<ExternalBuilders> externalBuilders` (the `Xp::ExternalBuilders` gate is commented out in source with a long explanatory note).

### Functions
- `template<> SandboxMode BaseSetting<SandboxMode>::parse(const std::string &) const` — declared.
- `template<> std::string BaseSetting<SandboxMode>::to_string() const` — declared.
- `template<> PathsInChroot BaseSetting<PathsInChroot>::parse(const std::string &) const` — declared.
- `template<> std::string BaseSetting<PathsInChroot>::to_string() const` — declared.
- `template<> void BaseSetting<PathsInChroot>::appendOrSet(PathsInChroot, bool)` — declared.
- `template<> LocalSettings::ExternalBuilders BaseSetting<LocalSettings::ExternalBuilders>::parse(const std::string &) const` — declared.
- `template<> std::string BaseSetting<LocalSettings::ExternalBuilders>::to_string() const` — declared.

### Type aliases
- `LocalSettings::ExternalBuilders = std::vector<ExternalBuilder>` (`using`).

### Macros / globals
- `const uint32_t maxIdsPerBuild = (Linux ? 1<<16 : 1)` — global constant used for `uidCount` defaults.

---

## File: src/libstore/globals.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `BaseSetting<SandboxMode>::trait` — full template specialisation in this TU: `static constexpr bool appendable = false`.
- `BaseSetting<std::vector<StoreReference>>::trait` — full template specialisation: `static constexpr bool appendable = true`.
- `BaseSetting<std::set<StoreReference>>::trait` — full template specialisation: `static constexpr bool appendable = true`.

### Functions

#### Free / static functions
- `Settings settings;` — global singleton.
- `static GlobalConfig::Register rSettings(&settings);` — file-local registration object.
- `Settings::Settings()` — constructor; resolves `nixStateDir` from `NIX_STATE_DIR` env / Windows known folders / build-time `NIX_STATE_DIR` macro, canonicalised; on non-Windows sets `buildUsersGroup` to `"nixbld"` if root else `""`; reads `NIX_IGNORE_SYMLINK_STORE` for `allowSymlinkedStore`; back-compat parses `NIX_REMOTE_SYSTEMS` into `builders`; on Linux/FreeBSD with `SANDBOX_SHELL` defined sets `sandboxPaths={"/bin/sh"=>SANDBOX_SHELL}`; on Apple seeds `sandboxPaths` and `allowedImpureHostPrefixes` with platform paths.
- `void loadConfFile(AbstractConfig & config)` — applies the system `nix.conf`, calls `config.resetOverridden()`, then iterates user conf files (XDG order) in reverse, finally applies `NIX_CONFIG`.
- `const std::filesystem::path & nixConfDir()` — runtime resolution (env `NIX_CONF_DIR` / Windows known folders / build-time macro).
- `const std::vector<std::filesystem::path> & nixUserConfFiles()` — list of user conf files (`NIX_USER_CONF_FILES` parsed with `ExecutablePath::parse`, otherwise XDG `getConfigDirs() / "nix.conf"`).
- `unsigned int Settings::getDefaultCores()` — returns `getMaxCPU()` if non-zero else `max(1, hardware_concurrency())`.
- `static bool hasVirt()` — Apple only; sysctl-based check (`kern.hv_vmm_present` and `kern.hv_support`).
- `StringSet Settings::getDefaultSystemFeatures()` — returns `{"nixos-test","benchmark","big-parallel"}`; Linux: adds `uid-range` and (when `/dev/kvm` is RW-accessible) `kvm`; Apple: adds `apple-virt` if `hasVirt()`.
- `StringSet Settings::getDefaultExtraPlatforms()` — Linux: when `NIX_LOCAL_SYSTEM == "x86_64-linux"` and not WSL1, adds `i686-linux`; on Linux always adds `<level>-linux` for each entry from `computeLevels()`. Apple aarch64: probes `arch -arch x86_64 /usr/bin/true` to decide whether to add `x86_64-darwin`.
- `bool Settings::isWSL1()` — `uname` suffix check (`-Microsoft`).
- `const ExternalBuilder * LocalSettings::findExternalDerivationBuilderIfSupported(const Derivation & drv)` — first matching handler whose `systems` set contains `drv.platform`, else `nullptr`.
- `ProfileDirsOptions Settings::getProfileDirsOptions() const` — packs `nixStateDir` and `useXDGBaseDirectories` into a `ProfileDirsOptions`.
- `std::string nixVersion = PACKAGE_VERSION;` — public version global.
- `NLOHMANN_JSON_SERIALIZE_ENUM(SandboxMode, ...)` — JSON enum mapping `smEnabled`↔`true`, `smRelaxed`↔`"relaxed"`, `smDisabled`↔`false`.
- `template<> SandboxMode BaseSetting<SandboxMode>::parse(...) const` — accepts `"true"`/`"relaxed"`/`"false"`.
- `template<> std::string BaseSetting<SandboxMode>::to_string() const` — inverse, with `unreachable()` on unknown values.
- `template<> void BaseSetting<SandboxMode>::convertToArg(Args & args, const std::string & category)` — adds `--<name>`, `--no-<name>`, `--relaxed-<name>` flags.
- `void to_json(nlohmann::json &, const ChrootPath &)` and `void from_json(const json &, ChrootPath &)` — `{source, optional}` shape.
- `template<> PathsInChroot BaseSetting<PathsInChroot>::parse(...) const` — parses whitespace-separated `inside=outside?` entries, with `?` marking optional sources.
- `template<> std::string BaseSetting<PathsInChroot>::to_string() const`.
- `unsigned int MaxBuildJobsSetting::parse(const std::string &) const` — `"auto"` → `max(1, hardware_concurrency())`, else integer (note: the `MaxBuildJobsSetting` class itself is defined in `worker-settings.hh`, outside this shard).
- `template<> LocalSettings::ExternalBuilders BaseSetting<LocalSettings::ExternalBuilders>::parse(...) const` — JSON parse with error wrapping.
- `template<> std::string BaseSetting<LocalSettings::ExternalBuilders>::to_string() const` — JSON dump.
- `template<> void BaseSetting<PathsInChroot>::appendOrSet(PathsInChroot, bool append)` — clears `value` when not appending; inserts via move iterators.
- `template<> StoreReference BaseSetting<StoreReference>::parse(...) const` — `StoreReference::parse`.
- `template<> std::string BaseSetting<StoreReference>::to_string() const` — `value.render()`.
- `template<> std::vector<StoreReference> BaseSetting<std::vector<StoreReference>>::parse(...) const`, `to_string()`, `appendOrSet(...)`.
- `template<> std::set<StoreReference> BaseSetting<std::set<StoreReference>>::parse(...) const`, `to_string()`, `appendOrSet(...)`.
- Explicit instantiations: `template class BaseSetting<StoreReference>;`, `template class BaseSetting<std::vector<StoreReference>>;`, `template class BaseSetting<std::set<StoreReference>>;`.
- `static void preloadNSS()` — glibc-only; `std::call_once`-guarded `dlopen(LIBNSS_DNS_SO, RTLD_NOW)` and `__nss_configure_lookup("hosts", "files dns")`.
- `static bool initLibStoreDone = false;` — file-scope guard.
- `void assertLibStoreInitialized()` — `printError`+`abort()` if `initNix()`/`initLibStore()` was not called.
- `void initLibStore(bool loadConfig)` — calls `initLibUtil`, optionally `loadConfFile(globalConfig)`, `preloadNSS`, `curl_global_init(CURL_GLOBAL_ALL)`, and on Apple unsets `TMPDIR` if it lives under `/var/folders/`.

### Type aliases
None new.

### Macros / globals
- `nix::Settings settings;` — global.
- `static GlobalConfig::Register rSettings(&settings);` — registration object.
- `std::string nixVersion = PACKAGE_VERSION;` — version global.
- `static bool initLibStoreDone` — file-local guard.

---

## File: src/libstore/include/nix/store/globals.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `ProfileDirsOptions` — forward declaration only.
- `LogFileSettings` — struct, `public virtual Config`. `Setting<bool> keepLog` (default true; alias `build-keep-log`), `Setting<bool> compressLog` (default true; alias `build-compress-log`).
- `NarInfoDiskCacheSettings` — struct, `public virtual Config`. `Setting<unsigned int> ttlNegative` (default 3600), `ttlPositive` (default 30 days), `ttlMeta` (default 7 days).
- `Settings` — class, `public virtual Config`, privately inherits `LocalSettings`, `LogFileSettings`, `WorkerSettings`, `NarInfoDiskCacheSettings`. Private static helpers `getDefaultSystemFeatures()`, `getDefaultExtraPlatforms()`, `isWSL1()`. Public constructor; getters `getLocalSettings()` (mut+const), `getLogFileSettings()` (mut+const), `getWorkerSettings()` (mut+const), `getNarInfoDiskCacheSettings()` (mut+const), static `getDefaultCores()`, `getProfileDirsOptions()`. Public data members: `std::filesystem::path nixStateDir`, `bool verboseBuild = true`, `bool readOnlyMode = false`. Public `Setting`s: `storeUri` (default from `NIX_REMOTE` or `"auto"`), `useSQLiteWAL` (default `!isWSL1()`), `keepFailed`, `thisSystem` (default `NIX_LOCAL_SYSTEM`), `trustedPublicKeys` (default `cache.nixos.org-1:...`; alias `binary-cache-public-keys`), `secretKeyFiles`, `requireSigs` (default true), `extraPlatforms`, `systemFeatures`, `trustedSubstituters` (alias `trusted-binary-caches`), `printMissing`, `useXDGBaseDirectories`, `warnLargePathThreshold`.

### Functions
- `extern nix::Settings settings;`
- `void loadConfFile(AbstractConfig & config)` — exported.
- `extern std::string nixVersion;`
- `void initLibStore(bool loadConfig = true)`.
- `void assertLibStoreInitialized()`.

### Type aliases
None new.

### Macros / globals
- `extern Settings settings;`
- `extern std::string nixVersion;`

---

## File: src/libstore/dummy-store.cc

### Namespaces
- `nix` (with an anonymous nested namespace for `WholeStoreViewAccessor`).
- `nlohmann` — re-opened to define `adl_serializer` specialisations.

### Classes / structs / enums
- `WholeStoreViewAccessor` — file-local class in an unnamed namespace, `: SourceAccessor`. Private nested alias `using BaseName = std::string`. Private members: `boost::concurrent_flat_map<BaseName, ref<MemorySourceAccessor>> subdirs`, `MemorySourceAccessor rootPathAccessor`, `MemorySourceAccessor emptyAccessor`. Private templated helper `callWithAccessorForPath(path, callback)`. Public default constructor (creates an empty root directory in `rootPathAccessor`). Public `addObject(baseName, accessor)`. Public overrides: `readFile`, `pathExists`, `maybeLstat`, `readDirectory` (single-arg version), `readLink`.
- `DummyStoreImpl` — struct (file-local in the outer `nix` namespace), `: DummyStore`. Private `void anchor() override`. Public alias `Config = DummyStoreConfig`. Member `ref<WholeStoreViewAccessor> wholeStoreView = make_ref<WholeStoreViewAccessor>()`. Constructor `(ref<const Config>)` (calls `Store{*config}` and `DummyStore{config}` and `wholeStoreView->setPathDisplay`). Overrides: `queryPathInfoUncached`, `isValidPathUncached`, `isTrustedClient` (`Trusted`), `queryPathFromHashPart` (throws via `unsupported`), `addToStore`, `addToStoreFromDump` (refuses derivation names), `writeDerivation`, `readDerivation`, `readInvalidDerivation` (delegates to `readDerivation`), `registerDrvOutput`, `queryRealisationUncached`, `getFSAccessor(StorePath, bool)`, `getFSAccessor(bool)` (returns the whole-store view). New helper `getMemoryFSAccessor(path, requireValidPath = true)` returning a `shared_ptr<MemorySourceAccessor>` (handles derivations by serialising via `unparse(*this, false)` on demand).

### Functions
- `DummyStoreConfig::anchor()` — empty.
- `DummyStore::anchor()` — empty.
- `DummyStoreImpl::anchor()` — empty.
- `DummyStoreConfig::doc()` — embeds `dummy-store.md`.
- `DummyStore::PathInfoAndContents::operator==(...)` — equality of `info` and the underlying memory tree (`contents->root`).
- `DummyStore::operator==(const DummyStore &)` — equality of `contents`, `derivations`, and `buildTrace`.
- `DummyStoreConfig::openStore() const` — calls `openDummyStore()`.
- `DummyStoreConfig::getReadOnly() const` — `readOnly.get() || StoreConfig::getReadOnly()`.
- `DummyStore::Config::openDummyStore() const` — `make_ref<DummyStoreImpl>(ref{shared_from_this()})`.
- `static RegisterStoreImplementation<DummyStore::Config> regDummyStore;` — registration.

#### `nlohmann::adl_serializer` specialisations (in nested `nlohmann` namespace)
- `adl_serializer<DummyStore::PathInfoAndContents>::from_json(json)` / `to_json(json, val)`.
- `adl_serializer<ref<DummyStore::Config>>::from_json(json)` (also forces `readOnly = true`).
- `adl_serializer<DummyStoreConfig>::to_json(json, val)`.
- `adl_serializer<ref<DummyStore>>::from_json(json)` (rebuilds `contents`/`derivations`/`buildTrace`).
- `adl_serializer<DummyStore>::to_json(json, val)`.

### Type aliases
- Inside `WholeStoreViewAccessor`: private `using BaseName = std::string`.

### Macros / globals
None new.

---

## File: src/libstore/include/nix/store/dummy-store.hh

### Namespaces
- `nix`
- `nlohmann` — to declare JSON serializer specialisations via `JSON_IMPL_INNER_TO`/`JSON_IMPL_INNER_FROM` macros.

### Classes / structs / enums
- `DummyStore` — forward declaration (`struct DummyStore;`).
- `DummyStoreConfig` — struct, `public std::enable_shared_from_this<DummyStoreConfig>, virtual StoreConfig`. Private `void anchor() override`. Two constructors: `(Params)` (sets `pathInfoCacheSize = 0`, uses `FilePathType::Unix`) and `(scheme, authority, params)` (delegates and throws `UsageError` if authority is non-empty). Public `Setting<bool> readOnly` (default true). Override `getReadOnly() const`. Static methods `name()` (`"Dummy Store"`), `doc()` (declared), `uriSchemes()` (`{"dummy"}`), `openDummyStore()`, override `openStore() const`. Inline override `getReference() const` returning `Specified{scheme="dummy"}` with the result of `getQueryParams()`.
- Trait specialisations `json_avoids_null<nix::DummyStoreConfig>`, `json_avoids_null<ref<nix::DummyStoreConfig>>`, `json_avoids_null<nix::DummyStore>`, `json_avoids_null<ref<nix::DummyStore>>` — each `: std::true_type`.

### Functions
- Macros `JSON_IMPL_INNER_TO(nix::DummyStoreConfig)`, `JSON_IMPL_INNER_FROM(nix::ref<nix::DummyStoreConfig>)`, `JSON_IMPL_INNER_TO(nix::DummyStore)`, `JSON_IMPL_INNER_FROM(nix::ref<nix::DummyStore>)` — emit the ADL serializer declarations in the `nlohmann` namespace.

### Type aliases
None new.

### Macros / globals
None new (uses external `JSON_IMPL_INNER_TO`/`JSON_IMPL_INNER_FROM`).

---

## File: src/libstore/include/nix/store/dummy-store-impl.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `MemorySourceAccessor` — forward declaration only.
- `DummyStore` — struct, `virtual Store`. Private `void anchor() override`. Public alias `Config = DummyStoreConfig`. Public member `ref<const Config> config`. Nested struct `PathInfoAndContents { UnkeyedValidPathInfo info; ref<MemorySourceAccessor> contents; bool operator==(const PathInfoAndContents &) const; }`. Public members: `boost::concurrent_flat_map<StorePath, PathInfoAndContents> contents`, `boost::concurrent_flat_map<StorePath, Derivation> derivations`, `boost::concurrent_flat_map<StorePath, std::map<std::string, UnkeyedRealisation>> buildTrace`. Inline constructor `(ref<const Config> config)` calling `Store{*config}` and storing the config. `bool operator==(const DummyStore &) const`.
- `json_avoids_null<DummyStore::PathInfoAndContents>` — full template specialisation `: std::true_type`.

### Functions
- Macro `JSON_IMPL(nix::DummyStore::PathInfoAndContents)` — emits ADL serializer declarations.

### Type aliases
- `DummyStore::Config = DummyStoreConfig` (`using`).

### Macros / globals
None new.

---

## File: src/libstore/restricted-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `RestrictedStore` — struct (file-local in this .cc), `public virtual IndirectRootStore, public virtual GcStore`. Private `void anchor() override`. Public members: `ref<const LocalStore::Config> config`, `ref<LocalStore> next`, `RestrictionContext & goal`. Constructor `(ref<LocalStore::Config>, ref<LocalStore>, RestrictionContext &)` initialises `Store`, `LocalFSStore`, and the three members. Public methods (override unless noted): `getRealStoreDir()` override returning `next->config->realStoreDir`, `queryAllValidPaths`, `queryPathInfoUncached`, `queryReferrers` (empty), `queryPartialDerivationOutputMap`, inline `queryPathFromHashPart` (throws), inline overload `addToStore(name, srcPath, ...)` (throws), `addToStore(info, narSource, repair, checkSigs)`, `addToStoreFromDump`, `narFromPath`, `ensurePath`, `registerDrvOutput`, `queryRealisationUncached`, `buildPaths`, `buildPathsWithResults`, inline `buildDerivation` (throws via `unsupported`), inline `addTempRoot` (no-op), inline `addIndirectRoot` (no-op), inline `findRoots` (returns empty `Roots()`), inline `collectGarbage` (no-op), inline `addSignatures` (`unsupported`), `queryMissing`, inline `getBuildLogExact` (returns nullopt), inline `addBuildLog` (`unsupported`), inline `isTrustedClient` (returns `NotTrusted`).

### Functions
- `static StorePath pathPartOfReq(const SingleDerivedPath & req)` — recursively unwraps a `SingleDerivedPath` to its leaf `StorePath`.
- `static StorePath pathPartOfReq(const DerivedPath & req)` — same for `DerivedPath`.
- `bool RestrictionContext::isAllowed(const DerivedPath & req)` — defined here (header declares it); calls `pathPartOfReq` then `isAllowed(StorePath)`.
- `void RestrictedStore::anchor() {}` — empty.
- `ref<Store> makeRestrictedStore(ref<LocalStore::Config> config, ref<LocalStore> next, RestrictionContext & context)` — factory; creates `make_ref<RestrictedStore>(config, next, context)`. (Note: the header declares this as taking `ref<LocalStoreConfig>`, which is the same as `LocalStore::Config`.)
- `RestrictedStore::queryAllValidPaths()` — `goal.originalPaths()` plus already-materialised entries from `goal.state_.lock()->addedPaths` (those whose future is `ready`).
- `RestrictedStore::queryPathInfoUncached(path, callback)` — censors `deriver`, `registrationTime`, `ultimate`, and `sigs`; returns nullptr for disallowed paths.
- `RestrictedStore::queryReferrers(...)` — empty.
- `RestrictedStore::queryPartialDerivationOutputMap(path, evalStore)` — guarded delegate to `next` (throws `InvalidPath` on disallowed).
- `RestrictedStore::addToStore(info, narSource, repair, checkSigs)` — delegate to `next`, then `goal.addDependency(info.path)`.
- `RestrictedStore::addToStoreFromDump(...)` — delegate to `next`, then `goal.addDependency(path)`.
- `RestrictedStore::narFromPath(path, sink)` — guarded `Store::narFromPath` (throws `InvalidPath` on disallowed).
- `RestrictedStore::ensurePath(path)` — assertion-only (allowed → no-op; otherwise throws).
- `RestrictedStore::registerDrvOutput(info)` — throws (XXX comment notes this should perhaps be a no-op for allowed derivations).
- `RestrictedStore::queryRealisationUncached(id, callback)` — calls `callback(nullptr)` if not allowed, then unconditionally forwards to `next->queryRealisation` (the source has no `else`/early-return after the disallowed-branch callback). XXX comment notes this should perhaps be allowed when the realisation corresponds to an allowed derivation.
- `RestrictedStore::buildPaths(paths, buildMode, evalStore)` — calls `buildPathsWithResults`, rethrowing per-result build errors.
- `RestrictedStore::buildPathsWithResults(paths, buildMode, evalStore)` — asserts no `evalStore`, refuses non-normal build modes; checks all paths are allowed; delegates to `next`; on success collects newly built outputs' closures and registers them via `goal.addDependency` and adds new realisations to `goal.state_.lock()->addedDrvOutputs`.
- `RestrictedStore::queryMissing(targets)` — partitions `targets` into allowed/unknown, delegates to `next->queryMissing` for the allowed ones, then merges the unknown set into the result.

### Type aliases
None new.

### Macros / globals
None new.

---

## File: src/libstore/include/nix/store/restricted-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `LocalStore` — forward declaration.
- `LocalStoreConfig` — forward declaration.
- `RestrictionContext` — struct. Pure virtual `originalPaths()`. Nested struct `State`: `std::map<StorePath, std::shared_future<void>> addedPaths`, `std::set<DrvOutput> addedDrvOutputs`. Member `Sync<State> state_`. Pure virtual `isAllowed(const StorePath &)`, `isAllowed(const DrvOutput &)`. Concrete inline `isAllowed(const DerivedPath &)` (definition in `restricted-store.cc`). Concrete inline `addDependency(path)` — coalesces concurrent additions through per-path promises/futures, calling `addDependencyImpl` once. Virtual `~RestrictionContext() = default`. Protected pure virtual `addDependencyImpl(path)`.

### Functions
- `ref<Store> makeRestrictedStore(ref<LocalStoreConfig> config, ref<LocalStore> next, RestrictionContext & context)` — declared.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libstore/schema.sql

### SQL schema

#### Tables
- `ValidPaths` — primary metadata table for store paths. Columns: `id` (integer, PRIMARY KEY AUTOINCREMENT, NOT NULL), `path` (text, UNIQUE, NOT NULL), `hash` (text, NOT NULL — base16 representation of the NAR sha256), `registrationTime` (integer, NOT NULL), `deriver` (text, nullable), `narSize` (integer, nullable), `ultimate` (integer, nullable; NULL implies false — "is locally produced and trusted"), `sigs` (text, nullable; space-separated), `ca` (text, nullable; CA assertion).
- `Refs` — references graph. Columns: `referrer` (integer, NOT NULL, FK → `ValidPaths(id)` ON DELETE CASCADE), `reference` (integer, NOT NULL, FK → `ValidPaths(id)` ON DELETE RESTRICT). PRIMARY KEY `(referrer, reference)`.
- `DerivationOutputs` — maps a derivation row to its named outputs. Columns: `drv` (integer, NOT NULL, FK → `ValidPaths(id)` ON DELETE CASCADE), `id` (text, NOT NULL; symbolic output id, usually `"out"`), `path` (text, NOT NULL). PRIMARY KEY `(drv, id)`.

#### Indexes
- `IndexReference` on `Refs(reference)` — speeds up looking up referrers. (Note: a stale `IndexReferrer` index that previously existed is dropped at runtime by the `20260309-drop-redundant-indexreferrer` migration in `LocalStore::upgradeDBSchema`.)
- `IndexDerivationOutputs` on `DerivationOutputs(path)` — speeds up "what derivation produced this output path?" queries.

#### Views
None.

#### Triggers
- `DeleteSelfRefs` — `BEFORE DELETE ON ValidPaths`; deletes any `(N, N)` self-references in `Refs` so that the `ON DELETE RESTRICT` foreign key on `reference` does not block deletion of the row.

---

## File: src/libstore/ca-specific-schema.sql

### SQL schema (loaded only when `Xp::CaDerivations` is enabled, applied by migration `20251017-ca-derivations`)

#### Tables
- `BuildTraceV3` — records realisation outcomes for content-addressing derivations. Columns: `id` (integer, PRIMARY KEY AUTOINCREMENT, NOT NULL), `drvPath` (text, NOT NULL — full store path of derivation), `outputName` (text, NOT NULL — symbolic output id, usually `"out"`), `outputPath` (text, NOT NULL), `signatures` (text, nullable; space-separated list). The header comment explains that the `*V<N>` naming convention preserves abandoned tables across experiment versions so users can switch versions (including downgrading) without migrating between them.

#### Indexes
- `IndexBuildTraceV3` on `BuildTraceV3(drvPath, outputName)` — composite index used by the `QueryRealisedOutput` lookup.

#### Views
None.

#### Triggers
None.

---

## Cross-file observations

### Boilerplate `anchor()` pinning
Every concrete class hierarchy in this shard provides a private `void anchor() override {}` whose only purpose is to give the vtable / typeinfo a definite TU. The pattern is repeated identically across `LocalStoreConfig`, `LocalBuildStoreConfig`, `LocalStore`, `LocalFSStoreConfig`, `LocalFSStore`, `LocalOverlayStoreConfig`, `LocalOverlayStore`, `IndirectRootStore`, `GcStore`, `DummyStoreConfig`, `DummyStore`, `DummyStoreImpl`, and `RestrictedStore` — twelve classes — and would be a natural target for a pinning macro.

### Layered overlay/lower-store delegation pattern
`LocalOverlayStore` repeats a structurally identical "try upper, fall through to lower" pattern in `queryPathInfoUncached`, `queryRealisationUncached`, `isValidPathUncached`, `queryPathFromHashPart`, `queryReferrers`, `queryValidDerivers`, and (with copy-up) `registerDrvOutput`/`registerValidPaths`. The two callback variants (`queryPathInfoUncached`, `queryRealisationUncached`) build a chained continuation through `Callback` and a captured `callbackPtr`; the synchronous variants short-circuit on the upper hit. A helper for the synchronous shape would consolidate four functions (`queryReferrers`, `queryValidDerivers`, `queryPathFromHashPart`, `isValidPathUncached`).

### Multiple "store wrapper" implementations
`LocalOverlayStore` (delegating to `lowerStore`), `RestrictedStore` (delegating to `next`), and `DummyStoreImpl` (no delegation; in-memory state) each redeclare large portions of the `Store` virtual surface. They share a "delegate guarded by a predicate" shape (e.g. `RestrictedStore::queryRealisationUncached` and `LocalOverlayStore::queryRealisationUncached` both filter or augment the underlying store's response via a callback wrapper). There is no shared abstract "delegating store" base.

### `Setting<...>` declarations across store configs
`LocalStoreConfig`, `LocalBuildStoreConfig`, `LocalOverlayStoreConfig`, `LocalFSStoreConfig`, `DummyStoreConfig`, `LocalSettings`, `GCSettings`, `AutoAllocateUidSettings`, `LogFileSettings`, `NarInfoDiskCacheSettings`, and `Settings` all use the same `Setting<T>{this, default, name, R"(doc)"}` initialiser format. The experimental-feature gate is consistently passed as a 6th positional argument when needed. `LocalFSStoreConfig::makeRootDirSetting` is a one-of-a-kind static helper that exists only to work around C++ virtual base init order — its comment explicitly calls out the design pain point.

### Path lookup & dispatch redundancy
`queryStaticPartialDerivationOutputMap`, `queryStaticPartialDerivationOutput`, `queryPartialDerivationOutputMap` form an overlapping family. `LocalStore` overrides only the static variants; `RestrictedStore` overrides the partial-output-map variant and merely guards it; `LocalOverlayStore` does not override these at all (it relies on the inherited `LocalStore` implementation). A consolidation pass might unify these flavours.

### Configuration parse/print specialisations
`globals.cc` carries a long sequence of `BaseSetting<X>::parse`/`to_string`/`appendOrSet`/`trait` template specialisations for `SandboxMode`, `PathsInChroot`, `LocalSettings::ExternalBuilders`, `StoreReference`, `std::vector<StoreReference>`, `std::set<StoreReference>`. Each follows the same "declare `trait::appendable`, then implement parse/to_string and (for collections) `appendOrSet`" shape. The `BaseSetting<SandboxMode>::trait` specialisation lives only in `globals.cc`; the `BaseSetting<PathsInChroot>::trait` specialisation lives in `local-settings.hh`. There is no shared template for "JSON-roundtripping setting" or "tokenise-list setting".

### GC vs runtime-roots vs auto-GC interaction
`local-gc.cc::findRuntimeRootsUnchecked`, `gc.cc::requestRuntimeRoots`, and `gc.cc::LocalStore::findRuntimeRoots` form three layers of similar logic. The first synthesises roots from `/proc` (or `lsof`); the second reads them from a Unix-domain socket served by an external roots daemon; and the third dispatches between the two based on `useRootsDaemon`. The `Roots` typedef (in `gc-store.hh`) and the file-local `UncheckedRoots` map (in `local-gc.cc`) reuse the same "store path → set of source labels" structure but with different key types (`StorePath` vs `std::string`); the comment in `local-gc.cc` notes the `std::string` key is forced by `std::filesystem::path` hashing being unavailable in macOS's libc++.

### Schema / migration coupling
`schema.sql.gen.hh` (generated from `schema.sql`) is `db.exec`'d only when `create=true` in `LocalStore::openDB`; whereas `ca-specific-schema.sql.gen.hh` is run via the named-migration mechanism in `upgradeDBSchema`. The two named migrations are `20251017-ca-derivations` and `20260309-drop-redundant-indexreferrer`; their idempotency rests on `create table if not exists` / `drop index if exists` plus the `SchemaMigrations` ledger. Adding new migrations requires editing `upgradeDBSchema`; new `BuildTraceV<N>` table versions need a fresh name to keep older clients functioning. The legacy in-line migrations for schemas <8/<9/<10 still live directly in the `LocalStore` constructor and bump the on-disk `schema` file rather than using the named-migration ledger.

### `LocalFSStore::drvsLogDir` and `addBuildLog`
`local-fs-store.cc` defines `drvsLogDir = "drvs"` (the static), and `LocalFSStore::getBuildLogExact` reads from `<logDir>/drvs/<aa>/<rest>` and `<logDir>/drvs/<full>` (and their `.bz2` variants), looping over both layouts. `local-store.cc::LocalStore::addBuildLog` writes only to `<logDir>/drvs/<aa>/<rest>.bz2`. The reader's compatibility loop and the writer's single hard-coded layout are not unified by a shared helper — adding a new layout requires touching both sides.

### `RestrictedStore`/`RestrictionContext` race coalescing
The header-defined `RestrictionContext::addDependency(path)` performs a per-path promise/future coalescing dance so that concurrent recursive-Nix dependency additions of the same path block on a single in-flight `addDependencyImpl` call. The abstract `addDependencyImpl` is the only point where the actual sandbox surfacing happens. This is the only such coalescing primitive in the shard; it is duplicated nowhere.

