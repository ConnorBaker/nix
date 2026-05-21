# Inventory — Shard 08: libstore remote stores (verified)

## File: src/libstore/remote-store.cc

### Namespaces
- `nix` — surrounds all definitions.

### Classes / structs / enums
- (none new defined here; this file implements `RemoteStore` and its inner types declared in headers.)

### Functions
- `RemoteStoreConfig::anchor()` — empty out-of-line anchor for vtable emission.
- `RemoteStore::RemoteStore(const Config & config)` — constructor; builds the `Pool<Connection>` whose factory calls `openConnectionWrapper` + `initConnection`, registers `connectionFds` for shutdown, and recycles connections only while `to.good()/from.good()` and steady-clock age below `maxConnectionAge` seconds.
- `RemoteStore::anchor()` — empty out-of-line anchor.
- `RemoteStore::openConnectionWrapper()` — wraps `openConnection`; if `failed` is already set, throws `Error("opening a connection ... previously failed")` after `checkInterrupt`; sets `failed` on exception.
- `RemoteStore::initConnection(Connection &)` — performs `WorkerProto::BasicClientConnection::handshake` (with `featureDisableSetOptions` injected into local version), checks against `WorkerProto::minimum.number`, stages `postHandshake` info, drains pending stderr exception, conditionally calls `setOptions(conn)` when feature absent.
- `RemoteStore::setOptions(Connection &)` — sends positional settings followed by an overrides map drawn from `settings`/`fileTransferSettings`, erasing those already serialised individually plus `showTrace`, `experimentalFeatures`, `plugin-files`.
- `RemoteStore::ConnectionHandle::~ConnectionHandle()` — marks the pooled connection bad if a non-`daemonException` exception is currently propagating.
- `RemoteStore::ConnectionHandle::processStderr(Sink*, Source*, bool flush, bool block)` — forwards to underlying `Connection::processStderr`, threading `daemonException` through.
- `RemoteStore::getConnection()` — pulls a `ConnectionHandle` from the pool.
- `RemoteStore::setOptions()` — overload calling `setOptions(*handle)` with a fresh handle.
- `RemoteStore::isValidPathUncached(const StorePath &)` — `WorkerProto::Op::IsValidPath`.
- `RemoteStore::queryValidPaths(const StorePathSet &, SubstituteFlag)` — delegates to `Connection::queryValidPaths`.
- `RemoteStore::queryAllValidPaths()` — `WorkerProto::Op::QueryAllValidPaths`.
- `RemoteStore::querySubstitutablePaths(const StorePathSet &)` — `WorkerProto::Op::QuerySubstitutablePaths`.
- `RemoteStore::querySubstitutablePathInfos(const StorePathCAMap &, SubstitutablePathInfos &)` — `WorkerProto::Op::QuerySubstitutablePathInfos`; below 1.22 sends a plain `StorePathSet` (drops CA hints).
- `RemoteStore::queryPathInfoUncached(const StorePath &, Callback<...>)` — wraps `Connection::queryPathInfo` and delivers a `ValidPathInfo` or null via callback.
- `RemoteStore::queryReferrers(const StorePath &, StorePathSet &)` — `WorkerProto::Op::QueryReferrers`.
- `RemoteStore::queryValidDerivers(const StorePath &)` — `WorkerProto::Op::QueryValidDerivers`.
- `RemoteStore::queryDerivationOutputs(const StorePath &)` — at protocol >=1.22 falls back to base `Store::queryDerivationOutputs`; otherwise `WorkerProto::Op::QueryDerivationOutputs`.
- `RemoteStore::queryPartialDerivationOutputMap(const StorePath &, Store *)` — at protocol >=1.22 either issues `QueryDerivationOutputMap` directly, or unions with eval-store static map; below 1.22 falls back to `evalStore.queryStaticPartialDerivationOutputMap`.
- `RemoteStore::queryPathFromHashPart(const std::string &)` — `WorkerProto::Op::QueryPathFromHashPart`.
- `RemoteStore::addCAToStore(Source &, std::string_view, ContentAddressMethod, HashAlgorithm, const StorePathSet &, RepairFlag)` — at >=1.25 sends `WorkerProto::Op::AddToStore` framed sink (with capacity bump); below 1.25 dispatches by `ContentAddressMethod::Raw` to `AddTextToStore` / `AddToStore` legacy variants and returns `queryPathInfo` after releasing the connection.
- `RemoteStore::addToStoreFromDump(Source &, std::string_view, FileSerialisationMethod, ContentAddressMethod, HashAlgorithm, const StorePathSet &, RepairFlag)` — translates `FileIngestionMethod` (Git is mapped to NAR), asserts dump matches, calls `addCAToStore`, then `invalidatePathInfoCacheFor`.
- `RemoteStore::addToStore(const ValidPathInfo &, Source &, RepairFlag, CheckSigsFlag)` — `WorkerProto::Op::AddToStoreNar` with framed-sink (>=1.23), source-streamed `processStderr` (>=1.21), or whole-NAR fallback.
- `RemoteStore::addMultipleToStore(PathsSource &&, Activity &, RepairFlag, CheckSigsFlag)` — `WorkerProto::Op::AddMultipleToStore` (>=1.32) using framed sink with reverse-iteration progress reporting; older protocols fall back to `Store::addMultipleToStore`.
- `RemoteStore::registerDrvOutput(const Realisation &)` — `WorkerProto::Op::RegisterDrvOutput`.
- `RemoteStore::queryRealisationUncached(const DrvOutput &, Callback<...>)` — gated on `WorkerProto::featureRealisationWithPath`; otherwise warns and returns null.
- `RemoteStore::copyDrvsFromEvalStore(const std::vector<DerivedPath> &, std::shared_ptr<Store>)` — copies `.drv` closures from a distinct eval store before `buildPaths`/`buildPathsWithResults`.
- `RemoteStore::buildPaths(const std::vector<DerivedPath> &, BuildMode, std::shared_ptr<Store>)` — `WorkerProto::Op::BuildPaths`.
- `RemoteStore::buildPathsWithResults(const std::vector<DerivedPath> &, BuildMode, std::shared_ptr<Store>)` — `WorkerProto::Op::BuildPathsWithResults` (>=1.34); else falls back to per-path synthesis via `buildPaths` + `resolveDerivedPath`/`queryRealisation`.
- `RemoteStore::buildDerivation(const StorePath &, const BasicDerivation &, BuildMode)` — calls `Connection::putBuildDerivationRequest` then reads `BuildResult`.
- `RemoteStore::ensurePath(const StorePath &)` — `WorkerProto::Op::EnsurePath`.
- `RemoteStore::addTempRoot(const StorePath &)` — delegates to `Connection::addTempRoot`.
- `RemoteStore::findRoots(bool censor)` — `WorkerProto::Op::FindRoots`.
- `RemoteStore::collectGarbage(const GCOptions &, GCResults &)` — branches on `WorkerProto::featureDeleteDeadSpecificReferrers`; legacy adapter visits `GCOptions::SpecificPaths`/`WholeStore` to flatten into a path set; clears `pathInfoCache` after run.
- `RemoteStore::optimiseStore()` — `WorkerProto::Op::OptimiseStore`.
- `RemoteStore::verifyStore(bool, RepairFlag)` — `WorkerProto::Op::VerifyStore`.
- `RemoteStore::addSignatures(const StorePath &, const std::set<Signature> &)` — `WorkerProto::Op::AddSignatures`.
- `RemoteStore::queryMissing(const std::vector<DerivedPath> &)` — `WorkerProto::Op::QueryMissing` (>=1.19); below 1.19 a `goto fallback` releases the handle and calls `Store::queryMissing`.
- `RemoteStore::addBuildLog(const StorePath &, std::string_view)` — `WorkerProto::Op::AddBuildLog` via framed sink.
- `RemoteStore::getVersion()` — returns `Connection::daemonNixVersion`.
- `RemoteStore::connect()` — opens and discards a connection.
- `RemoteStore::getProtocol()` — returns `protoVersion.number.toWire()` (uses the raw pool, not `getConnection()`).
- `RemoteStore::isTrustedClient()` — returns `Connection::remoteTrustsUs`.
- `RemoteStore::flushBadConnections()` — `connections->flushBad()`.
- `RemoteStore::shutdownConnections()` — calls `::shutdown(toSocket(fd), SHUT_RDWR)` on every tracked descriptor.
- `RemoteStore::narFromPath(const StorePath &, Sink &)` — `Connection::narFromPath` with a callback that `copyNAR`s through the connection.
- `RemoteStore::getRemoteFSAccessor(bool)` — `make_ref<RemoteFSAccessor>(ref<Store>(shared_from_this()), requireValidPath)`.
- `RemoteStore::getFSAccessor(bool)` — returns `getRemoteFSAccessor(...)`.
- `RemoteStore::getFSAccessor(const StorePath &, bool)` — calls `getRemoteFSAccessor(...)->accessObject(path)`.
- `RemoteStore::ConnectionHandle::withFramedSink(fun<void(Sink&)>)` — flushes `to`, runs caller's lambda inside a `FramedSink` whose flush callback invokes `processStderr(nullptr, nullptr, false, false)`, then drains stderr with `flush=false`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/remote-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- forward decls: `class Pipe;`, `class Pid;`, `struct FdSink;`, `struct FdSource;`, `template<typename T> class Pool;`, `class RemoteFSAccessor;`.
- `struct RemoteStoreConfig : virtual StoreConfig` — daemon-style store config.
- `struct RemoteStore : public virtual Store, public virtual GcStore, public virtual LogStore` — abstract base with inner forward declarations `struct Connection;` and `struct ConnectionHandle;`. Friends `struct ConnectionHandle`. Derived stores subclass `RemoteStore` to implement `openConnection` over different transports.

### Functions
- `RemoteStoreConfig::anchor()` (private override, declared).
- `RemoteStoreConfig::RemoteStoreConfig(const Params &, FilePathType)` — inline ctor forwarding to `StoreConfig`.
- `RemoteStore::anchor()` (private override, declared).
- public ctor `RemoteStore(const Config &)`.
- public overrides: `isValidPathUncached`, `queryValidPaths`, `queryAllValidPaths`, `queryPathInfoUncached`, `queryReferrers`, `queryValidDerivers`, `queryDerivationOutputs`, `queryPartialDerivationOutputMap`, `queryPathFromHashPart`, `querySubstitutablePaths`, `querySubstitutablePathInfos`, `addToStoreFromDump`, `addToStore` (info+source variant), `addMultipleToStore`, `registerDrvOutput`, `queryRealisationUncached`, `buildPaths`, `buildPathsWithResults`, `buildDerivation`, `ensurePath`, `addTempRoot`, `findRoots`, `collectGarbage`, `optimiseStore`, `verifyStore`, `repairPath` (inline override that calls `unsupported("repairPath")`), `addSignatures`, `queryMissing`, `addBuildLog`, `getVersion`, `connect`, `getProtocol`, `isTrustedClient`.
- public non-virtual member `addCAToStore` (helper used by `addToStoreFromDump`).
- public helpers: `flushBadConnections()`, `shutdownConnections()`, `openConnectionWrapper()`.
- protected pure-virtual `openConnection() = 0`.
- protected helpers `initConnection(Connection &)`, virtual `setOptions(Connection &)`, `setOptions()` override, `getConnection()`.
- protected overrides `getFSAccessor(bool)`, `getFSAccessor(const StorePath &, bool)`, `narFromPath(const StorePath &, Sink &)`.
- private `getRemoteFSAccessor(bool)`, `copyDrvsFromEvalStore`.

### Type aliases
- `RemoteStore::Config = RemoteStoreConfig` (using-declaration).

### Macros / globals
- `Setting<int> RemoteStoreConfig::maxConnections` — default 1, name `"max-connections"`.
- `Setting<unsigned int> RemoteStoreConfig::maxConnectionAge` — default `numeric_limits<unsigned int>::max()`, name `"max-connection-age"`.
- Member fields: `const Config & config`, `ref<Pool<Connection>> connections`, `std::atomic_bool failed{false}`, `Sync<std::set<Descriptor>> connectionFds`.

---

## File: src/libstore/include/nix/store/remote-store-connection.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct RemoteStore::Connection : WorkerProto::BasicClientConnection, WorkerProto::ClientHandshakeInfo` — adds `std::chrono::time_point<std::chrono::steady_clock> startTime` for max-age tracking.
- `struct RemoteStore::ConnectionHandle` — RAII wrapper carrying `Pool<RemoteStore::Connection>::Handle handle` and `bool daemonException = false`; defines ctor (move from handle), move ctor (noexcept), destructor (declared; defined in remote-store.cc), `operator*`, `operator->`, `processStderr`, `withFramedSink`.

### Functions
- `ConnectionHandle::ConnectionHandle(Pool<...>::Handle &&)` — inline ctor.
- `ConnectionHandle::ConnectionHandle(ConnectionHandle &&)` noexcept — inline move ctor.
- `~ConnectionHandle()` — declared, defined in `.cc`.
- `ConnectionHandle::operator*()` and `operator->()` — inline.
- `processStderr(Sink* = 0, Source* = 0, bool flush = true, bool block = true)` — declared, defined in `.cc`.
- `withFramedSink(fun<void(Sink&)>)` — declared, defined in `.cc`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/uds-remote-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none new.)

### Functions
- `getDaemonSocketPath(const Store::Config &)` — free function returning `$NIX_DAEMON_SOCKET_PATH` (when non-empty) else `<stateDir>/daemon-socket/socket`.
- `UDSRemoteStoreConfig::anchor()`, `UDSRemoteStore::anchor()` — empty out-of-line anchors.
- `UDSRemoteStoreConfig::UDSRemoteStoreConfig(const std::filesystem::path &, const StoreReference::Params &)` — ctor; initialises `Store::Config{params, FilePathType::Native}`, `LocalFSStore::Config{params}`, `RemoteStore::Config{params, FilePathType::Native}` and stores `path` (defaulting via `getDaemonSocketPath` when empty).
- `UDSRemoteStoreConfig::doc()` — embeds `uds-remote-store.md`.
- `UDSRemoteStoreConfig::UDSRemoteStoreConfig(const Params &)` — delegating ctor passing empty path.
- `UDSRemoteStore::UDSRemoteStore(ref<const Config>)` — store ctor initialising `Store`, `LocalFSStore`, `RemoteStore` bases plus `config`.
- `UDSRemoteStoreConfig::getReference()` — returns `Daemon` variant when `path == getDaemonSocketPath(*this)`, else `Specified{scheme=*uriSchemes().begin(), authority=encodeUrlPath(pathToUrlPath(path))}`.
- `UDSRemoteStore::Connection::closeWrite()` — `shutdown(toSocket(fd.get()), SHUT_WR)`.
- `UDSRemoteStore::openConnection()` — `nix::connect(config->path)`, wires `from.fd`/`to.fd` and `startTime`.
- `UDSRemoteStore::addIndirectRoot(const std::filesystem::path &)` — `WorkerProto::Op::AddIndirectRoot`.
- `UDSRemoteStore::Config::openStore()` — `make_ref<UDSRemoteStore>(ref{shared_from_this()})`.

### Type aliases
- (none new in this TU; `UDSRemoteStore::Config = UDSRemoteStoreConfig` is declared in the header.)

### Macros / globals
- `static RegisterStoreImplementation<UDSRemoteStore::Config> regUDSRemoteStore` — registers URI scheme `unix`.

---

## File: src/libstore/include/nix/store/uds-remote-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct UDSRemoteStoreConfig : std::enable_shared_from_this<UDSRemoteStoreConfig>, virtual LocalFSStoreConfig, virtual RemoteStoreConfig` — adds public `std::filesystem::path path`. Statics `name()` (returns `"Local Daemon Store"`) and `uriSchemes()` (`{"unix"}`); `doc()`. Override `openStore`, `getReference`. Private `anchor()`.
- `struct UDSRemoteStore : virtual IndirectRootStore, virtual RemoteStore` — concrete store; private inner `struct Connection : RemoteStore::Connection { AutoCloseFD fd; void closeWrite() override; }`. Public `ref<const Config> config`. Inline overrides:
  - `getFSAccessor(bool)` -> `LocalFSStore::getFSAccessor(requireValidPath)`.
  - `getFSAccessor(const StorePath &, bool)` -> `LocalFSStore::getFSAccessor(path, requireValidPath)`.
  - `narFromPath(const StorePath &, Sink &)` -> `Store::narFromPath(path, sink)`.
  - `addIndirectRoot(const std::filesystem::path &)` (declared, defined in `.cc`).
  - private `openConnection() override`.

### Functions
- `getDaemonSocketPath(const Store::Config &)` declared.
- ctors `UDSRemoteStoreConfig(path, params)` and `UDSRemoteStoreConfig(params)` declared.
- ctor `UDSRemoteStore(ref<const Config>)` declared.

### Type aliases
- `UDSRemoteStore::Config = UDSRemoteStoreConfig`.

### Macros / globals
- (none.)

---

## File: src/libstore/binary-cache-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none new.)

### Functions
- `BinaryCacheStoreConfig::anchor()`, `BinaryCacheStore::anchor()` — empty out-of-line anchors.
- `BinaryCacheStore::BinaryCacheStore(Config &)` — loads `secretKeyFile` then comma-separated `secretKeyFiles` into `signers`, primes `narMagic` from `narVersionMagic1`.
- `BinaryCacheStore::init()` — calls `getNixCacheInfo`; if absent writes `StoreDir: <storeDir>\n`; if present iterates lines, validates `StoreDir`, applies `WantMassQuery` and `Priority` defaults via `setDefault`.
- `BinaryCacheStore::getNixCacheInfo()` — `getFile(cacheInfoFile)`.
- `BinaryCacheStore::upsertFile(const std::string &, std::string &&, const std::string &, uint64_t)` — wraps in `StringSource` and forwards to subclass `upsertFile`.
- `BinaryCacheStore::getFile(const std::string &, Callback<std::optional<std::string>>) noexcept` — synchronous default driving the sync `getFile` and forwarding the result.
- `BinaryCacheStore::getFile(const std::string &, Sink &)` — pumps the callback-based `getFile` through a `std::promise`/`std::future` and writes the resulting data to sink.
- `BinaryCacheStore::getFile(const std::string &)` — convenience returning `std::optional<std::string>`; swallows `NoSuchBinaryCacheFile`.
- `BinaryCacheStore::narInfoFileFor(const StorePath &)` — `${hashPart}.narinfo`.
- `BinaryCacheStore::writeNarInfo(ref<NarInfo>)` — uploads `.narinfo`, upserts `pathInfoCache`, propagates to `diskCache->upsertNarInfo`.
- `BinaryCacheStore::addToStoreCommon(Source &, RepairFlag, CheckSigsFlag, fun<ValidPathInfo(HashResult)>)` — central NAR ingest pipeline:
  - tees through `CompressionSink` (parallel auto-enabled when zstd unless overridden) -> `FdSink`+`fileHashSink`+`narHashSink` and parses NAR listing into a `NarAccessor`;
  - chooses `narInfo->url` per-compression suffix (xz/bz2/zst/lzip/lz4/br);
  - validates references via `queryPathInfo`;
  - optionally writes `<hash>.ls` JSON listing;
  - optionally indexes `lib/debug/.build-id/<2>/<38>.debug` into `debuginfo/<build-id>` JSON via 25-thread `ThreadPool`;
  - uploads `.nar` (only when missing or `repair`) and bumps `narWrite/narWriteAverted/narWriteBytes/narWriteCompressedBytes/narWriteCompressionTimeMs`;
  - signs via `narInfo->sign(*this, signers)`;
  - calls `writeNarInfo`.
- `BinaryCacheStore::addToStore(const ValidPathInfo &, Source &, RepairFlag, CheckSigsFlag)` — drains source if path already valid, else feeds `addToStoreCommon` returning `info` unchanged.
- `BinaryCacheStore::addToStoreFromDump(...)` — rejects Git ingestion (`unsupported`); when given a `StringSource` builds replayable NAR (Flat is `dumpString`-wrapped) and computes CA hash; otherwise requires NAR+SHA-256; calls `addToStoreCommon` building a `ValidPathInfo` via `ValidPathInfo::makeFromCA`.
- `BinaryCacheStore::isValidPathUncached(const StorePath &)` — `fileExists(narInfoFileFor(...))`.
- `BinaryCacheStore::queryPathFromHashPart(const std::string &)` — synthesises `StorePath(hashPart + "-" + MissingName)` and returns `queryPathInfo(...)->path`, returning `nullopt` on `InvalidPath`.
- `BinaryCacheStore::narFromPath(const StorePath &, Sink &)` — fetches `info->url` through a decompression sink that updates `narRead`/`narReadBytes`; rethrows `NoSuchBinaryCacheFile` as `SubstituteGone`.
- `BinaryCacheStore::queryPathInfoUncached(const StorePath &, Callback<...>) noexcept` — async; opens `Activity actQueryPathInfo`, downloads `.narinfo`, parses into `NarInfo` via `(*this, *data, narInfoFile)` ctor, increments `narInfoRead`.
- `BinaryCacheStore::addToStore(name, const SourcePath &, ...)` — hash-then-stream variant: pre-computes `hashPath`, builds NAR via `path.dumpPath(sink, filter)`, and feeds `addToStoreCommon`.
- `BinaryCacheStore::makeRealisationPath(const DrvOutput &)` — `realisationsPrefix + "/" + drvPath.to_string() + "/" + outputName + ".doi"`.
- `BinaryCacheStore::queryRealisationUncached(const DrvOutput &, Callback<...>) noexcept` — fetches realisation JSON via inner `Callback<std::optional<std::string>>` and parses into `UnkeyedRealisation`.
- `BinaryCacheStore::registerDrvOutput(const Realisation &)` — upserts disk cache, uploads JSON to realisation path.
- `BinaryCacheStore::getRemoteFSAccessor(bool)` — `make_ref<RemoteFSAccessor>(ref<Store>(shared_from_this()), requireValidPath, config.localNarCache)`.
- `BinaryCacheStore::getFSAccessor(bool)` and `getFSAccessor(const StorePath &, bool)` — wire to the remote FS accessor.
- `BinaryCacheStore::addSignatures(const StorePath &, const std::set<Signature> &)` — clones existing `NarInfo`, merges sigs, re-uploads via `writeNarInfo`.
- `BinaryCacheStore::getBuildLogExact(const StorePath &)` — `getFile("log/" + baseNameOf(printStorePath(path)))`.
- `BinaryCacheStore::addBuildLog(const StorePath &, std::string_view)` — uploads `log/<drvPath>` as `text/plain; charset=utf-8`; asserts `drvPath.isDerivation()`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/binary-cache-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- forward decls: `struct NarInfo;`, `class RemoteFSAccessor;`.
- `struct BinaryCacheStoreConfig : virtual StoreConfig` — public ctor `(const Params &)` calling `StoreConfig(params, FilePathType::Unix)`. Settings: `compression` (`Setting<CompressionAlgo>` default `xz`), `writeNARListing` (`write-nar-listing`), `writeDebugInfo` (`index-debug-info`), `secretKeyFile` (`secret-key`, optional), `secretKeyFiles` (`secret-keys`, comma list), `localNarCache` (optional), `parallelCompression` (default false), `compressionLevel` (default -1). Private `anchor()`.
- `struct alignas(8) BinaryCacheStore : virtual Store, virtual LogStore` — abstract base with mutable `Config & config` reference. Private `anchor()`, `signers` vector. Protected: static constexpr `realisationsPrefix = "build-trace-v2"`, `cacheInfoFile = "nix-cache-info"`; ctor `BinaryCacheStore(Config &)`; helper `makeRealisationPath`. Pure virtuals: `fileExists(const std::string &)`, `upsertFile(path, RestartableSource &, mimeType, sizeHint)`. Virtuals (with default impls): `getFile(path, Sink &)`, `getNixCacheInfo()`, `getFile(path, Callback<std::optional<std::string>>) noexcept`. Inline overload `upsertFile(path, std::string &&, mimeType)` defers to size-aware overload; declared `upsertFile(path, std::string &&, mimeType, sizeHint)`. Single-arg `getFile(path)` returning `std::optional<std::string>`. Public overrides: `init`, `isValidPathUncached`, `queryPathInfoUncached`, `queryPathFromHashPart`, `addToStore` (info+source), `addToStoreFromDump`, `addToStore` (SourcePath variant), `registerDrvOutput`, `queryRealisationUncached`, `narFromPath`, `getFSAccessor`×2, `addSignatures`, `getBuildLogExact`, `addBuildLog`. Private members: `narMagic`, `narInfoFileFor`, `writeNarInfo`, `addToStoreCommon`, `getRemoteFSAccessor`.
- `MakeError(NoSuchBinaryCacheFile, Error)` — error class macro at namespace scope.

### Functions
- All public overrides listed above.

### Type aliases
- `BinaryCacheStore::Config = BinaryCacheStoreConfig` (using-declaration).

### Macros / globals
- `MakeError(NoSuchBinaryCacheFile, Error)` macro expansion.
- Static constexpr `realisationsPrefix`, `cacheInfoFile` (member, not free).

---

## File: src/libstore/local-binary-cache-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `struct LocalBinaryCacheStore : virtual BinaryCacheStore` — file-local concrete store. Members:
  - public `using Config = LocalBinaryCacheStoreConfig;`, `ref<Config> config`.
  - inline ctor `LocalBinaryCacheStore(ref<Config>)` initialising `Store{*config}` and `BinaryCacheStore{*config}`.
  - `init()` override.
  - protected `fileExists(const std::string &)` override.
  - protected inline `upsertFile(const std::string &, RestartableSource &, const std::string & mimeType, uint64_t sizeHint)` override — calls `checkBinaryCachePath`, creates parent dirs, writes to a temp file `<path>.tmp.<pid>.<counter>` via `AutoDelete`, then `std::filesystem::rename` and `del.cancel()`. The `counter` is a function-local `static std::atomic<int>`.
  - protected inline `getFile(const std::string &, Sink &)` override — `readFile(checkBinaryCachePath(...), sink)`, translating `std::errc::no_such_file_or_directory` to `NoSuchBinaryCacheFile`.
  - protected inline `queryAllValidPaths()` override — iterates `DirectoryIterator{binaryCacheDir}`, accepts entries whose 40-char filename ends with `.narinfo`, and reconstructs `parseStorePath(storeDir + "/" + name.substr(0, name.size()-8) + "-" + MissingName)`.
  - protected inline `isTrustedClient()` override — returns `Trusted`.
  - private `anchor()` override.

### Functions
- `static checkBinaryCachePath(const std::filesystem::path & root, const std::string & path)` — rejects empty / absolute / `..` / `.` segments and returns `root / p.relative_path()`.
- `LocalBinaryCacheStoreConfig::LocalBinaryCacheStoreConfig(const std::filesystem::path &, const StoreReference::Params &)` — initialises `Store::Config{params, FilePathType::Unix}` and `BinaryCacheStoreConfig{params}` and stores `binaryCacheDir`.
- `LocalBinaryCacheStoreConfig::doc()` — embeds `local-binary-cache-store.md`.
- `LocalBinaryCacheStoreConfig::getReference()` — `Specified{scheme="file", authority=encodeUrlPath(pathToUrlPath(binaryCacheDir))}` (no params).
- `LocalBinaryCacheStore::init()` — out-of-line; creates `nar/`, `realisationsPrefix`, `debuginfo/` (if `writeDebugInfo`), `log/` then calls base `BinaryCacheStore::init()`.
- `LocalBinaryCacheStore::fileExists(const std::string &)` — out-of-line.
- `LocalBinaryCacheStoreConfig::uriSchemes()` — returns `{}` if `_NIX_FORCE_HTTP=1`, otherwise `{"file"}`.
- `LocalBinaryCacheStoreConfig::anchor()`, `LocalBinaryCacheStore::anchor()` — empty.
- `LocalBinaryCacheStoreConfig::openStore()` — `make_ref<LocalBinaryCacheStore>(...)` with `const_pointer_cast` and runs `store->init()`.

### Type aliases
- `LocalBinaryCacheStore::Config = LocalBinaryCacheStoreConfig` (file-local class).

### Macros / globals
- `static RegisterStoreImplementation<LocalBinaryCacheStore::Config> regLocalBinaryCacheStore`.

---

## File: src/libstore/include/nix/store/local-binary-cache-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct LocalBinaryCacheStoreConfig : std::enable_shared_from_this<LocalBinaryCacheStoreConfig>, virtual Store::Config, BinaryCacheStoreConfig` — public `std::filesystem::path binaryCacheDir`. Static `name()` (`"Local Binary Cache Store"`), `uriSchemes()`, `doc()`. Override `openStore`, `getReference`. Private `anchor()`.

### Functions
- inline ctor `LocalBinaryCacheStoreConfig(const Params &)` initialising `StoreConfig(params, FilePathType::Unix)` and `BinaryCacheStoreConfig(params)`.
- ctor `LocalBinaryCacheStoreConfig(const std::filesystem::path &, const Params &)` declared.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/http-binary-cache-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `MakeError(UploadToHTTP, Error)` — error type.

### Functions
- `HttpBinaryCacheStoreConfig::uriSchemes()` — `{"http","https"}` plus `"file"` when env `_NIX_FORCE_HTTP=1` (cached in a function-local static).
- `HttpBinaryCacheStoreConfig::anchor()`, `HttpBinaryCacheStore::anchor()`.
- `HttpBinaryCacheStoreConfig::HttpBinaryCacheStoreConfig(ParsedURL, const Params &)` — initialises `StoreConfig(params, FilePathType::Unix)` and `BinaryCacheStoreConfig(params)`; throws `UsageError` if scheme is not `"file"` and authority/host empty; trims trailing empty path segments.
- `HttpBinaryCacheStoreConfig::getReference()` — `Specified{scheme=cacheUri.scheme, authority=cacheUri.renderAuthorityAndPath()}` plus `getQueryParams()`.
- `HttpBinaryCacheStoreConfig::doc()` — embeds `http-binary-cache-store.md`.
- `HttpBinaryCacheStore::HttpBinaryCacheStore(ref<Config>, ref<FileTransfer>)` — initialises bases plus `fileTransfer`/`config`, sets `diskCache` via `NarInfoDiskCache::get` using `getNarInfoDiskCacheSettings()`/`{useWAL=settings.useSQLiteWAL}`.
- `HttpBinaryCacheStore::init()` — uses `diskCache->upToDateCacheExists(cacheKey)` to short-circuit; otherwise invokes `BinaryCacheStore::init()` (mapping `UploadToHTTP` to `Error("'%s' does not appear to be a binary cache")`) and calls `diskCache->createCache(...)`.
- `HttpBinaryCacheStore::topoSortPaths(const StorePathSet &)` — async closure traversal using `computeClosure<StorePath>` with `callbackToAwaitable<...>(queryPathInfo)` and `topoSort`, throwing on `Cycle<StorePath>`.
- `HttpBinaryCacheStore::getCompressionMethod(const std::string &)` — returns per-suffix compression algo for `.narinfo` (`narinfoCompression`), `.ls` (`lsCompression`), `log/...` (`logCompression`).
- `HttpBinaryCacheStore::maybeDisable()` — if enabled and `tryFallback`, prints error and disables for 60s via `_state`.
- `HttpBinaryCacheStore::checkEnabled()` — re-enables once past `disabledUntil` or throws `SubstituterDisabled`.
- `HttpBinaryCacheStore::fileExists(const std::string &)` — HEAD request; treats `FileTransfer::NotFound` and `FileTransfer::Forbidden` (S3 unlistable) as missing; calls `maybeDisable` on other errors.
- `HttpBinaryCacheStore::upload(std::string_view, RestartableSource &, uint64_t, std::string_view, std::optional<Headers>)` — internal PUT helper that builds the request via `makeRequest`, attaches `data`/`mimeType` and any extra headers, calls `fileTransfer->upload`.
- `HttpBinaryCacheStore::upsertFile(const std::string &, RestartableSource &, const std::string &, uint64_t)` — chooses optional content compression (`getCompressionMethod`), wraps compressed body as a `StringSource`, sets `Content-Encoding`, dispatches `upload`; rewraps `FileTransferError` as `UploadToHTTP` with traced URI.
- `HttpBinaryCacheStore::makeRequest(std::string_view path)` — builds `FileTransferRequest` from `parseURLRelative(path, cacheUri+/)`; preserves S3 query params from base URI when relative result has none; attaches TLS cert/key when scheme+authority match base; propagates per-substituter retry overrides via local `propagate` lambda checking `setting.isOverridden()`.
- `HttpBinaryCacheStore::getFile(const std::string &, Sink &)` — synchronous download with `maybeDisable` on error; translates `NotFound`/`Forbidden` to `NoSuchBinaryCacheFile`.
- `HttpBinaryCacheStore::getFile(const std::string &, Callback<std::optional<std::string>>) noexcept` — async via `enqueueFileTransfer`; on `NotFound`/`Forbidden` returns empty value, otherwise rethrows after `maybeDisable`.
- `HttpBinaryCacheStore::getNixCacheInfo()` — `download(makeRequest(cacheInfoFile))` returning `result.data`; `nullopt` on 404; calls `maybeDisable` on other errors.
- `HttpBinaryCacheStore::isTrustedClient()` — returns `nullopt` (HTTP authentication status not exposed yet).
- `HttpBinaryCacheStore::Config::openStore(ref<FileTransfer>)` — typed factory.
- `HttpBinaryCacheStoreConfig::openStore() const` — calls overload with `getFileTransfer()`.

### Type aliases
- (none.)

### Macros / globals
- `static RegisterStoreImplementation<HttpBinaryCacheStore::Config> regHttpBinaryCacheStore`.
- `MakeError(UploadToHTTP, Error)` macro at namespace scope.

---

## File: src/libstore/include/nix/store/http-binary-cache-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct HttpBinaryCacheStoreConfig : std::enable_shared_from_this<HttpBinaryCacheStoreConfig>, virtual Store::Config, BinaryCacheStoreConfig` — public `ParsedURL cacheUri`. Settings: `narinfoCompression` (`Setting<std::optional<CompressionAlgo>>`), `lsCompression`, `logCompression`, `tlsCert` (optional `AbsolutePath`), `tlsKey`, `retryDelayMs` (`uint32_t`, default 0), `retryDelayRateLimitedMs`, `retryMaxDelayMs`, `retryAttempts`. Statics `name()` (`"HTTP Binary Cache Store"`), `uriSchemes()`, `doc()`. Virtuals `openStore(ref<FileTransfer>)` (non-override) and `openStore() override`, `getReference() override`. Private `anchor()`. Two ctors: `(const Params &)` and `(ParsedURL, const Store::Config::Params &)`.
- `class HttpBinaryCacheStore : public virtual BinaryCacheStore` — private `void anchor() override;`. Inner `struct State { bool enabled = true; std::chrono::steady_clock::time_point disabledUntil; };`. Private `Sync<State> _state`. Protected `ref<FileTransfer> fileTransfer`. Public `using Config = HttpBinaryCacheStoreConfig;` and `ref<Config> config`. Public ctor `HttpBinaryCacheStore(ref<Config>, ref<FileTransfer> = getFileTransfer())`. Public overrides `init()` and `topoSortPaths(const StorePathSet &)`. Protected helpers `getCompressionMethod`, `maybeDisable`, `checkEnabled`. Protected overrides `fileExists`, `upsertFile`. Protected helpers `makeRequest`, `upload`. Protected overrides `getFile(const std::string &, Sink &)`, `getFile(const std::string &, Callback<std::optional<std::string>>) noexcept`, `getNixCacheInfo`, `isTrustedClient`.

### Functions
- ctor `HttpBinaryCacheStore(ref<Config>, ref<FileTransfer> = getFileTransfer())` declared.
- All overrides above.

### Type aliases
- `HttpBinaryCacheStore::Config = HttpBinaryCacheStoreConfig`.

### Macros / globals
- (none.)

---

## File: src/libstore/s3-binary-cache-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `class S3BinaryCacheStore : public virtual HttpBinaryCacheStore` — file-local concrete store:
  - public ctor `S3BinaryCacheStore(ref<S3BinaryCacheStoreConfig>)` initialising `Store{*config}`, `BinaryCacheStore{*config}`, `HttpBinaryCacheStore{config}` and `s3Config{config}`.
  - public `upsertFile(const std::string &, RestartableSource &, const std::string &, uint64_t)` override — runs an inner `doUpload` lambda that drains source through a `HashSink(MD5)`, validates length, attaches `Content-MD5` (Base64) and optional `x-amz-storage-class`, and dispatches to `uploadMultipart` (when `multipartUpload` and size above `multipartThreshold`) or `upload`; outer body chooses optional compression and translates `FileTransferError` to `UploadToS3`.
  - private `ref<S3BinaryCacheStoreConfig> s3Config`.
  - private `upload(std::string_view, RestartableSource &, uint64_t, std::string_view, std::optional<Headers>)` — debug logs, rejects sizes above `AWS_MAX_PART_SIZE`, then delegates to `HttpBinaryCacheStore::upload`.
  - private `uploadMultipart(...)` — instantiates `MultipartSink`, drains source, calls `sink.finish()`.
  - private inner `struct MultipartSink : Sink` with members `S3BinaryCacheStore & store`, `std::string_view path`, `std::string uploadId`, `std::string::size_type chunkSize`, `std::vector<std::string> partEtags`, `std::string buffer`. Methods: ctor (computes `chunkSize`, auto-adjusts when `estimatedParts > AWS_MAX_PART_COUNT`, calls `createMultipartUpload`), `operator()` (chunked accumulation), `finish` (calls `completeMultipartUpload`, aborts on error), `uploadChunk` (calls `uploadPart`, aborts on error).
  - private `createMultipartUpload(std::string_view, std::string_view, std::optional<Headers>)` — POST `?uploads`, regex-extracts `<UploadId>`.
  - private `uploadPart(std::string_view, std::string_view, uint64_t, std::string)` — PUT with `partNumber` and `uploadId` query params, returns `result.etag`.
  - private `completeMultipartUpload(std::string_view, std::string_view, std::span<const std::string>)` — POST XML payload listing `<Part>` elements.
  - private `abortMultipartUpload(...) noexcept` — DELETE with `uploadId`, swallowing exceptions via `ignoreExceptionInDestructor`.
- `MakeError(UploadToS3, Error)`.

### Functions
- `S3BinaryCacheStoreConfig::uriSchemes()` -> `{"s3"}`.
- `S3BinaryCacheStoreConfig::S3BinaryCacheStoreConfig(ParsedURL, const Params &)` — asserts query empty / scheme `s3`, copies params named in `s3UriSettings` into `cacheUri.query`, validates `multipartChunkSize` bounds, warns when `multipartUpload && multipartThreshold < multipartChunkSize`.
- `S3BinaryCacheStoreConfig::S3BinaryCacheStoreConfig(std::string_view bucketName, const Params &)` — bucket-only ctor delegating to ParsedURL ctor.
- `S3BinaryCacheStoreConfig::getHumanReadableURI()` — preserves only overridden `s3UriSettings` in the rendered URI.
- `S3BinaryCacheStoreConfig::doc()` — embeds `s3-binary-cache-store.md`.
- `S3BinaryCacheStoreConfig::openStore()` — `const_pointer_cast`+`static_pointer_cast` then `make_ref<S3BinaryCacheStore>`.
- `S3BinaryCacheStore::upsertFile`, `upload`, `uploadMultipart` definitions.
- `MultipartSink` ctor / `operator()` / `finish` / `uploadChunk` definitions.
- `createMultipartUpload`, `uploadPart`, `abortMultipartUpload`, `completeMultipartUpload` definitions.

### Type aliases
- (none.)

### Macros / globals
- `static constexpr uint64_t AWS_MIN_PART_SIZE = 5 * 1024 * 1024;`
- `static constexpr uint64_t AWS_MAX_PART_SIZE = 5ULL * 1024 * 1024 * 1024;`
- `static constexpr uint64_t AWS_MAX_PART_COUNT = 10000;`
- `static RegisterStoreImplementation<S3BinaryCacheStoreConfig> registerS3BinaryCacheStore`.
- `MakeError(UploadToS3, Error)`.

---

## File: src/libstore/include/nix/store/s3-binary-cache-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct S3BinaryCacheStoreConfig : HttpBinaryCacheStoreConfig` — settings:
  - `profile` (default `"default"`), `region` (default `"us-east-1"`), `scheme` (`"https"`), `endpoint` (empty), `addressingStyle` (`S3AddressingStyle::Auto`).
  - `multipartUpload` (default false), `multipartChunkSize` (`uint64_t`, default 5 MiB, alias `buffer-size`), `multipartThreshold` (default 100 MiB).
  - `storageClass` (`Setting<std::optional<std::string>>`).
  - const `s3UriSettings = {&profile, &region, &scheme, &endpoint, &addressingStyle}` (`std::set<const AbstractSetting *>`).
  - statics `name()` (`"S3 Binary Cache Store"`), `uriSchemes()`, `doc()`.
  - overrides `getHumanReadableURI`, `openStore`.

### Functions
- Three ctors: `(const Params &)`, `(ParsedURL, const Params &)`, `(std::string_view bucketName, const Params &)`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/s3-url.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none new.)

### Functions
- `parseS3AddressingStyle(std::string_view)` — `auto`/`path`/`virtual` decoder; throws `InvalidS3AddressingStyle` otherwise.
- `showS3AddressingStyle(S3AddressingStyle)` — switch returning a `string_view`; calls `unreachable()` for unknown values.
- `ParsedS3URL::parse(const ParsedURL &)` — validates `scheme == "s3"`, requires registered-name authority for the bucket, requires non-empty key; extracts query params (`endpoint`, `profile`, `region`, `scheme`, `versionId`, `addressing-style`); the `endpoint` becomes `monostate` when absent, `ParsedURL` if it parses as a full URL, else `ParsedURL::Authority`. Adds traces to `BadURL`/`InvalidS3AddressingStyle`.
- `ParsedS3URL::toHttpsUrl()` — converts to HTTPS, choosing virtual-vs-path style with dotted-bucket fallback (warns once via static `warnedDottedBucket`); handles three endpoint variants (`monostate` AWS hosts, `ParsedURL::Authority`, full `ParsedURL`) via `std::visit`.
- `to_json(nlohmann::json &, const S3AddressingStyle &)` and `from_json(const nlohmann::json &, S3AddressingStyle &)` — JSON ADL.
- `template<> S3AddressingStyle BaseSetting<S3AddressingStyle>::parse(const std::string &) const` — translates `InvalidS3AddressingStyle` into `UsageError`.
- `template<> std::string BaseSetting<S3AddressingStyle>::to_string() const`.
- explicit instantiation `template class BaseSetting<S3AddressingStyle>;`.

### Type aliases
- `template<> struct json_avoids_null<S3AddressingStyle> : std::true_type {};` — specialisation in `nix` namespace.
- `template<> struct BaseSetting<S3AddressingStyle>::trait { static constexpr bool appendable = false; };` — nested-trait specialisation.

### Macros / globals
- `using namespace std::string_view_literals;` (file scope).

---

## File: src/libstore/include/nix/store/s3-url.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `enum class S3AddressingStyle { Auto, Path, Virtual };`
- `MakeError(InvalidS3AddressingStyle, Error)`.
- `struct ParsedS3URL` — fields `bucket`, `key` (vector), optional `profile`, `region`, `scheme`, `versionId`, `addressingStyle`, `endpoint` (`std::variant<std::monostate, ParsedURL, ParsedURL::Authority>`). Methods: inline `getEncodedEndpoint()` returning `optional<string>` via `std::visit`, static `parse`, `toHttpsUrl`, defaulted `operator<=>`.

### Functions
- `parseS3AddressingStyle`, `showS3AddressingStyle` declarations.

### Type aliases
- (none.)

### Macros / globals
- `NIX_DECLARE_CONFIG_SERIALISER(S3AddressingStyle)` macro.

---

## File: src/libstore/aws-creds.cc

### Namespaces
- `nix` (with anonymous nested namespace for helpers).

### Classes / structs / enums (file is `#if NIX_WITH_AWS_AUTH`)
- `class AwsCredentialProviderImpl : public AwsCredentialProvider` — overrides `getCredentials(const ParsedS3URL &)`. Holds `Aws::Crt::ApiHandle apiHandle`, `std::shared_ptr<Aws::Crt::Io::TlsContext> tlsContext`, raw `Aws::Crt::Io::ClientBootstrap * bootstrap`, and `boost::concurrent_flat_map<std::pair<string,string>, std::shared_ptr<ICredentialsProvider>> credentialProviderCache`.

### Functions
- `AwsAuthError::AwsAuthError(int errorCode)` — formats `aws_error_str(errorCode)` and stores `errorCode`.
- (anonymous) `awsLogLevelToVerbosity(enum aws_log_level) -> Verbosity` — maps AWS levels conservatively (FATAL -> `lvlError`; ERROR/WARN/INFO -> `lvlDebug`; DEBUG/TRACE -> `lvlVomit`; NONE/COUNT -> `lvlDebug`).
- (anonymous) `nixAwsLoggerLog(struct aws_logger *, enum aws_log_level, aws_log_subject_t, const char * format, ...)` — custom AWS logger that formats via `vsnprintf` and routes through `printMsgUsing(nix::logger, ...)`.
- (anonymous) `nixAwsLoggerGetLevel(struct aws_logger *, aws_log_subject_t)` — maps Nix verbosity back to AWS level.
- (anonymous) `initialiseAwsLogger()` — installs the vtable+logger once via `std::call_once`.
- (anonymous) `createWrappedProvider(aws_credentials_provider *, allocator)` — wraps a C provider in `Aws::Crt::Auth::ICredentialsProvider`.
- (anonymous) `createSSOProvider(profile, bootstrap, tlsContext, allocator)`.
- (anonymous) `awsRegionSetInEnv()` — checks `AWS_REGION` / `AWS_DEFAULT_REGION` non-empty.
- (anonymous) `createSTSWebIdentityProvider(profile, fallbackRegion, bootstrap, tlsContext, allocator)`.
- (anonymous) `createECSProvider(bootstrap, tlsContext, allocator)`.
- (anonymous) `getCredentialsFromProvider(provider)` — synchronous wait with 30s timeout via `std::promise`/`std::future`.
- `AwsCredentialProviderImpl::AwsCredentialProviderImpl()` — sets up logger, TLS context (warns and clears on failure), bootstrap.
- `AwsCredentialProviderImpl::getCredentialsRaw(const std::string &, const std::string &)` — uses `try_emplace_and_cvisit` keyed by `(profile, region)`; on null provider erases the cache entry and throws.
- `AwsCredentialProviderImpl::getCredentials(const ParsedS3URL &)` — invokes `getCredentialsRaw`, evicts cache entry on `AwsAuthError`.
- `AwsCredentialProviderImpl::createProviderForProfile(const std::string &, const std::string &)` — builds chain Environment -> SSO -> Profile -> STS WebIdentity -> ECS -> IMDS (ECS and IMDS mutually exclusive); skips SSO/STS/ECS when no `tlsContext`.
- `makeAwsCredentialsProvider()` — `make_ref<AwsCredentialProviderImpl>()`.
- `getAwsCredentialsProvider()` — returns global singleton via function-local static.

### Type aliases
- (none.)

### Macros / globals
- nested static `nixAwsLoggerVtable`, `nixAwsLogger`, `initialised` (`std::once_flag`), `warnedDottedBucket` are local statics within their respective functions; no namespace-scope globals.

---

## File: src/libstore/include/nix/store/aws-creds.hh

### Namespaces
- `nix`

### Classes / structs / enums (inside `#if NIX_WITH_AWS_AUTH`)
- `struct AwsCredentials` — `accessKeyId`, `secretAccessKey`, `optional<sessionToken>`; ctor accepting all three (sessionToken defaulted to `nullopt`).
- `class AwsAuthError final : public CloneableError<AwsAuthError, Error>` — private `std::optional<int> errorCode`. `using CloneableError::CloneableError;`. Adds ctor `AwsAuthError(int)` and `getErrorCode()`.
- `class AwsCredentialProvider` — abstract base with pure virtual `getCredentials(const ParsedS3URL &)`, helper `maybeGetCredentials(const ParsedS3URL &)` that swallows `AwsAuthError`, and virtual destructor.

### Functions
- `makeAwsCredentialsProvider()` declared.
- `getAwsCredentialsProvider()` declared.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/ssh.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `class InvalidSSHAuthority final : public CloneableError<InvalidSSHAuthority, Error>` — file-local error type with ctor taking `(const ParsedURL::Authority &, std::string_view reason)`.

### Functions
- `static parsePublicHostKey(std::string_view host, std::string_view sshPublicHostKey)` — Base64-decode with traced error.
- `static checkValidAuthority(const ParsedURL::Authority &)` — validates user/host non-empty and not starting with `-`; throws `InvalidSSHAuthority`.
- `getNixSshOpts()` — splits `NIX_SSHOPTS` env var via `shellSplitString` into `OsStrings`; traces failures.
- `SSHMaster::SSHMaster(const ParsedURL::Authority &, std::optional<std::filesystem::path> keyFile, std::string_view sshPublicHostKey, bool useMaster, bool compress, Descriptor logFD)` — initialises authority, `hostnameAndUser`, `fakeSSH = (authority.to_string() == "localhost")`, raw `sshPublicHostKey` decoded via `parsePublicHostKey`, `useMaster && !fakeSSH`, `tmpDir = make_ref<AutoDelete>(createTempDir(...))`. Calls `checkValidAuthority`.
- `SSHMaster::addCommonSSHOpts(OsStrings &)` — appends `NIX_SSHOPTS`, `-i <keyFile>`, `-oUserKnownHostsFile=<tmpDir>/host-key` (writing `<host> <pub>\n`), `-C` (compress), `-p<port>`, `-oPermitLocalCommand=yes`, `-oLocalCommand=echo started`.
- `SSHMaster::isMasterRunning()` — runs `ssh -O check <hostnameAndUser>` via `runProgram` and returns true on rc 0.
- `Strings createSSHEnv()` (file-local helper) — copies env and sets `SHELL=/bin/sh`.
- `SSHMaster::startCommand(OsStrings && command, OsStrings && extraSshArgs)` — Windows: throws `UnimplementedError`. Otherwise calls `startMaster()`, creates two pipes, forks via `startProcess` with `dieWithParent=false`, dups input/output/log FDs, runs `ssh <user@host> -x [opts] [-S socket] [-v] [extra] -- command...`. When not `fakeSSH`/`useMaster`, suspends the logger and waits for `started` reply on stdout.
- `SSHMaster::startMaster()` (Linux/macOS only) — runs `ssh -M -N -S <socket>` on the locked state's master pipe; waits for `"started"` confirmation. No-ops if `useMaster` is false.
- `SSHMaster::Connection::trySetBufferSize(size_t)` — guarded by `F_SETPIPE_SZ`; asserts `size <= INT_MAX` and applies `fcntl(...)` to both `in` and `out`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/ssh.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class SSHMaster` — private members: `ParsedURL::Authority authority`, `std::string hostnameAndUser`, `bool fakeSSH`, `const std::optional<std::filesystem::path> keyFile`, `const std::string sshPublicHostKey` (raw bytes), `const bool useMaster`, `const bool compress`, `const Descriptor logFD`, `const ref<const AutoDelete> tmpDir`. Inner `struct State { Pid sshMaster; std::filesystem::path socketPath; }` (`Pid` only on non-Windows). `Sync<State> state_`. Helpers `addCommonSSHOpts`, `isMasterRunning`, `startMaster` (non-Windows). Public ctor `(authority, keyFile, sshPublicHostKey, useMaster, compress, logFD = INVALID_DESCRIPTOR)` and `startCommand(OsStrings && command, OsStrings && extraSshArgs = {})`.
- `struct SSHMaster::Connection` — non-Windows only `Pid sshPid`; `AutoCloseFD out, in`; `void trySetBufferSize(size_t)`.

### Functions
- `getNixSshOpts()` declared.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/ssh-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `struct alignas(8) SSHStore : virtual RemoteStore` — file-local concrete store. Public `using Config = SSHStoreConfig;`, `ref<const Config> config`. Inline ctor initialising bases + `master` (uses SSH master only when `connections->capacity() > 1`). Public override `getBuildLogExact(const StorePath &)` calling `unsupported("getBuildLogExact")`. Protected inner `struct Connection : RemoteStore::Connection` with `std::unique_ptr<SSHMaster::Connection> sshConn` and inline `closeWrite()` calling `sshConn->in.close()`. Protected `openConnection()` override; protected member `extraRemoteProgramArgs` (`std::vector<std::string>`); protected `SSHMaster master`; protected inline `setOptions(RemoteStore::Connection &)` override (no-op with TODO comment). Private `anchor()`.
- `struct MountedSSHStore : virtual SSHStore, virtual LocalFSStore` — file-local. Public `using Config = MountedSSHStoreConfig;`. Inline ctor sets `extraRemoteProgramArgs = {"--process-ops"}`. Inline overrides:
  - `narFromPath(const StorePath &, Sink &)` -> `Store::narFromPath`.
  - `getFSAccessor(bool)` and `getFSAccessor(const StorePath &, bool)` -> `LocalFSStore::getFSAccessor`.
  - `getBuildLogExact(const StorePath &)` -> `LocalFSStore::getBuildLogExact`.
  - `addPermRoot(const StorePath &, const std::filesystem::path &)` -> sends `WorkerProto::Op::AddPermRoot` and reads back the new root path.
  Private `anchor()`.

### Functions
- `SSHStoreConfig::SSHStoreConfig(const ParsedURL::Authority &, const Params &)` — initialises `Store::Config{params, FilePathType::Unix}`, `RemoteStore::Config{params, FilePathType::Unix}`, `CommonSSHStoreConfig{authority, params}`.
- `SSHStoreConfig::anchor()`, `MountedSSHStoreConfig::anchor()`, `SSHStore::anchor()`, `MountedSSHStore::anchor()` — empty.
- `SSHStoreConfig::doc()` — embeds `ssh-store.md`.
- `SSHStoreConfig::getReference()` — `Specified{scheme=*uriSchemes().begin(), authority=authority.to_string()}` plus `getQueryParams()`.
- `MountedSSHStoreConfig::MountedSSHStoreConfig(StringMap params)` and `MountedSSHStoreConfig::MountedSSHStoreConfig(const ParsedURL::Authority &, StringMap params)` — initialise the diamond (`StoreConfig`, `RemoteStoreConfig`, `CommonSSHStoreConfig`, `SSHStoreConfig`, `LocalFSStoreConfig`).
- `MountedSSHStoreConfig::doc()` — embeds `mounted-ssh-store.md`.
- `SSHStore::Config::openStore()` — `make_ref<SSHStore>(ref{shared_from_this()})`.
- `MountedSSHStore::Config::openStore()` — `make_ref<MountedSSHStore>` after `dynamic_pointer_cast`.
- `SSHStore::openConnection()` — assembles `Strings command = config->remoteProgram.get() + ["--stdio"] [+ "--store" + remoteStore] + extraRemoteProgramArgs`, pipes via `master.startCommand(toOsStrings(...))`, wires `FdSink`/`FdSource`.

### Type aliases
- `SSHStore::Config = SSHStoreConfig` (declared inside class).
- `MountedSSHStore::Config = MountedSSHStoreConfig`.

### Macros / globals
- `static RegisterStoreImplementation<SSHStore::Config> regSSHStore;`
- `static RegisterStoreImplementation<MountedSSHStore::Config> regMountedSSHStore;`

---

## File: src/libstore/include/nix/store/ssh-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct SSHStoreConfig : std::enable_shared_from_this<SSHStoreConfig>, virtual RemoteStoreConfig, virtual CommonSSHStoreConfig` — `Setting<Strings> remoteProgram` (default `{"nix-daemon"}`); statics `name()` (`"Experimental SSH Store"`), `uriSchemes()` (`{"ssh-ng"}`), `doc()`; virtuals `openStore`, `getReference`. Inline ctor `(const Params &)`; ctor `(const ParsedURL::Authority &, const Params &)` declared. Private `anchor()`.
- `struct MountedSSHStoreConfig : virtual SSHStoreConfig, virtual LocalFSStoreConfig` — statics `name()` (`"Experimental SSH Store with filesystem mounted"`), `uriSchemes()` (`{"mounted-ssh-ng"}`), `doc()`, `experimentalFeature()` returning `ExperimentalFeature::MountedSSHStore`. Override `openStore`. Two ctors `(StringMap)` and `(const ParsedURL::Authority &, StringMap)`. Private `anchor()`.

### Functions
- ctors declared.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/legacy-ssh-store.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `struct LegacySSHStore::Connection : public ServeProto::BasicClientConnection` — adds `std::unique_ptr<SSHMaster::Connection> sshConn` and `bool good = true`.

### Functions
- `LegacySSHStoreConfig::LegacySSHStoreConfig(const ParsedURL::Authority &, const Params &)` — initialises `StoreConfig(params, FilePathType::Unix)` and `CommonSSHStoreConfig(authority, params)`.
- `LegacySSHStoreConfig::anchor()`, `LegacySSHStore::anchor()` — empty.
- `LegacySSHStoreConfig::doc()` — embeds `legacy-ssh-store.md`.
- `LegacySSHStore::LegacySSHStore(ref<const Config>)` — builds connection pool with `r->good` filter; constructs `SSHMaster` via `config->createSSHMaster(connections->capacity() > 1, config->logFD)`.
- `LegacySSHStore::openConnection()` — runs `nix-store --serve --write [--store ...]` and `extraSshArgs`, optionally `trySetBufferSize`, performs `ServeProto::BasicClientConnection::handshake(to, tee, ServeProto::latest, host)`, translates `SerialisationError`/`EndOfFile` into traced errors.
- `LegacySSHStoreConfig::getReference()` — `Specified{scheme=*uriSchemes().begin(), authority=authority.to_string()}` plus `getQueryParams()`.
- `LegacySSHStore::queryPathInfosUncached(const StorePathSet &)` — batched path info via `Connection::queryPathInfos`; throws if any `narHash == Hash::dummy`.
- `LegacySSHStore::queryPathInfoUncached(const StorePath &, Callback<...>)` — wraps `queryPathInfosUncached({path})`, returns null/single/throws on multiple results.
- `LegacySSHStore::addToStore(const ValidPathInfo &, Source &, RepairFlag, CheckSigsFlag)` — writes `ServeProto::Command::AddToStoreNar` followed by `copyNAR`; on exception sets `conn->good = false`. Reads back `1` for success.
- `LegacySSHStore::narFromPath(const StorePath &, Sink &)` — delegates to the lambda overload via `copyNAR`.
- `LegacySSHStore::narFromPath(const StorePath &, fun<void(Source &)>)` — calls `Connection::narFromPath` (Hydra-friendly).
- `static buildSettings()` (file-local) — extracts `ServeProto::BuildOptions` from globals (`maxSilentTime`, `buildTimeout`, `maxLogSize`, `nrRepeats=0`, `enforceDeterminism=0`, `keepFailed`).
- `LegacySSHStore::buildDerivation(const StorePath &, const BasicDerivation &, BuildMode)` — calls `putBuildDerivationRequest` then `getBuildDerivationResponse`.
- `LegacySSHStore::buildDerivationAsync(const StorePath &, const BasicDerivation &, const ServeProto::BuildOptions &)` — captures `Pool<Connection>::Handle` in a `shared_ptr`, returning a `fun<BuildResult()>` that calls `getBuildDerivationResponse` (single-shot).
- `LegacySSHStore::buildPaths(const std::vector<DerivedPath> &, BuildMode, std::shared_ptr<Store>)` — sends `ServeProto::Command::BuildPaths`; rejects derived paths that aren't `StorePathWithOutputs`; reads back `BuildResultStatus` and throws `BuildError` on failure.
- `LegacySSHStore::computeFSClosure(const StorePathSet &, StorePathSet &, bool, bool, bool)` — uses `ServeProto::Command::QueryClosure`; falls back to `Store::computeFSClosure` when `flipDirection || includeDerivers`.
- `LegacySSHStore::queryValidPaths(const StorePathSet &, SubstituteFlag)` — base override delegating to `Connection::queryValidPaths(*this, false, paths, maybeSubstitute)`.
- `LegacySSHStore::queryValidPaths(const StorePathSet &, bool lock, SubstituteFlag)` — extra-lock variant for Hydra.
- `LegacySSHStore::connect()` — opens and discards a connection.
- `LegacySSHStore::getProtocol()` — returns `conn->remoteVersion.toWire()`.
- `LegacySSHStore::getConnectionPid()` — returns `sshConn->sshPid` on non-Windows, `0` otherwise (TODO).
- `LegacySSHStore::getConnectionStats()` — returns `{bytesReceived = conn->from.read, bytesSent = conn->to.written}`.
- `LegacySSHStore::isTrustedClient()` — always `nullopt`.
- `LegacySSHStore::Config::openStore()` — `make_ref<LegacySSHStore>(ref{shared_from_this()})`.

### Type aliases
- (none.)

### Macros / globals
- `static RegisterStoreImplementation<LegacySSHStore::Config> regLegacySSHStore;`

---

## File: src/libstore/include/nix/store/legacy-ssh-store.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct LegacySSHStoreConfig : std::enable_shared_from_this<LegacySSHStoreConfig>, virtual CommonSSHStoreConfig` — `logFD` is `Setting<int>` on non-Windows (default `INVALID_DESCRIPTOR`, name `"log-fd"`) and a plain `Descriptor logFD = INVALID_DESCRIPTOR` on Windows; `Setting<Strings> remoteProgram` (default `{"nix-store"}`); `Setting<int> maxConnections` (default 1); Hydra hooks `Strings extraSshArgs = {}`, `std::optional<size_t> connPipeSize`. Statics `name()` (`"SSH Store"`), `uriSchemes()` (`{"ssh"}`), `doc()`. Virtuals `openStore`, `getReference`. Two ctors. Private `anchor()`.
- `struct LegacySSHStore : public virtual Store` — forward-declared inner `struct Connection;`. Public `using Config = LegacySSHStoreConfig;`, `ref<const Config> config`, `ref<Pool<Connection>> connections`, `SSHMaster master`. Public ctor `(ref<const Config>)` and `openConnection()`. Overrides:
  - `queryPathInfoUncached`, `addToStore` (info+source variant), `narFromPath` (×2 incl. Hydra-only `fun<void(Source &)>` overload).
  - `queryPathFromHashPart` -> `unsupported`.
  - `addToStore(name, SourcePath, ...)` -> `unsupported`.
  - `addToStoreFromDump` -> `unsupported`.
  - `registerDrvOutput` -> `unsupported`.
  - `buildDerivation`, `buildDerivationAsync` (returns `fun<BuildResult()>`), `buildPaths`.
  - `ensurePath` -> `unsupported`.
  - `getFSAccessor` (×2) -> `unsupported`.
  - `repairPath` -> `unsupported`.
  - `computeFSClosure`.
  - `queryValidPaths` (override + extra `lock` variant).
  - `connect`, `getProtocol`, `isTrustedClient`.
  - `queryRealisationUncached` -> `unsupported` (declared as override).
  - `querySubstitutablePaths` -> returns empty.
  - `queryPathInfosUncached(const StorePathSet &)` (non-virtual helper used by the callback overload).
- Inner `struct ConnectionStats { size_t bytesReceived, bytesSent; };` and public `getConnectionStats()`, `getConnectionPid()`. Private `anchor()`.

### Functions
- ctor `LegacySSHStore(ref<const Config>)` declared.

### Type aliases
- `LegacySSHStore::Config = LegacySSHStoreConfig`.

### Macros / globals
- (none.)

---

## File: src/libstore/common-ssh-store-config.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none new.)

### Functions
- `CommonSSHStoreConfig::CommonSSHStoreConfig(const ParsedURL::Authority &, const Params &)` — initialises `StoreConfig(params, FilePathType::Unix)` and stores `authority`.
- `CommonSSHStoreConfig::anchor()` — empty.
- `CommonSSHStoreConfig::createSSHMaster(bool useMaster, Descriptor logFD) const` — constructs `SSHMaster{authority, sshKey.get(), sshPublicHostKey.get(), useMaster, compress, logFD}`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/common-ssh-store-config.hh

### Namespaces
- `nix`

### Classes / structs / enums
- forward decl `class SSHMaster;`.
- `struct CommonSSHStoreConfig : virtual StoreConfig` — settings `sshKey` (`Setting<std::optional<AbsolutePath>>`), `sshPublicHostKey` (`Setting<std::string>` base64), `compress` (`Setting<bool>`, default false), `remoteStore` (`Setting<std::string>`); public `ParsedURL::Authority authority`; helper `createSSHMaster(bool useMaster, Descriptor logFD = INVALID_DESCRIPTOR) const`. Two ctors `(const Params &)` (inline) and `(const ParsedURL::Authority &, const Params &)` (declared). Private `anchor()`.

### Functions
- ctors declared.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/remote-fs-accessor.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none new.)

### Functions
- `RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool requireValidPath, std::optional<AbsolutePath> cacheDir)` — initialiser list `store(store), narCache(cacheDir), requireValidPath(requireValidPath)`; body empty.
- `RemoteFSAccessor::fetch(const CanonPath & path)` — calls `store->toStorePath(store->storeDir + path.abs())`; if `requireValidPath` and `!store->isValidPath(storePath)` throws `InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath))`; returns `{ref{accessObject(storePath)}, restPath}`.
- `RemoteFSAccessor::accessObject(const StorePath & storePath)` — `if (auto * narHash = get(narHashes, storePath.hashPart()))` returns `narCache.getOrInsert(*narHash, [&](Sink & sink) { store->narFromPath(storePath, sink); })`; else queries `store->queryPathInfo(storePath)`, emplaces `narHashes[storePath.hashPart()] = info->narHash`, returns same `narCache.getOrInsert` keyed on `info->narHash`.
- `RemoteFSAccessor::maybeLstat(const CanonPath & path)` — short-circuits root with `Stat{.type = tDirectory}`; else delegates via `fetch`.
- `RemoteFSAccessor::readDirectory(const CanonPath & path)` — delegates via `fetch`.
- `RemoteFSAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)` — delegates via `fetch`.
- `RemoteFSAccessor::readLink(const CanonPath & path)` — delegates via `fetch`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/remote-fs-accessor.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class RemoteFSAccessor : public SourceAccessor` — private fields `ref<Store> store`, `std::map<std::string, Hash, std::less<>> narHashes` (hash-part to NAR-hash indirection), `NarCache narCache`, `bool requireValidPath`; private `fetch(const CanonPath & path)` returning `std::pair<ref<SourceAccessor>, CanonPath>`; `friend struct BinaryCacheStore`. Public: `accessObject(const StorePath & path)` returning `std::shared_ptr<SourceAccessor>`; ctor `(ref<Store> store, bool requireValidPath = true, std::optional<AbsolutePath> cacheDir = {})`; overrides `maybeLstat`, `readDirectory`, `readFile(path, sink, sizeCallback)`, `readLink`; `using SourceAccessor::readFile;` to bring base overloads into scope.

### Functions
- (declared above; see .cc.)

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/nar-info.cc

### Namespaces
- `nix` (definitions for `NarInfo` and `UnkeyedNarInfo`).
- `nlohmann` (specialisations of `adl_serializer<UnkeyedNarInfo>`).

### Classes / structs / enums
- (none new.)

### Functions
- `NarInfo::NarInfo(const StoreDirConfig & store, const std::string & s, const std::string & whence)` — diamond ctor delegating into `UnkeyedValidPathInfo(store, Hash::dummy)` (FIXME hack), `ValidPathInfo(StorePath::dummy, static_cast<const UnkeyedValidPathInfo &>(*this))` (FIXME hack), `UnkeyedNarInfo(static_cast<const UnkeyedValidPathInfo &>(*this))`. Walks `s` line-by-line, splitting on `':'` and `'\n'`; recognises fields `StorePath`, `URL`, `Compression`, `FileHash`, `FileSize`, `NarHash`, `NarSize`, `References`, `Deriver` (with `unknown-deriver` sentinel), `Sig`, `CA`. Internal lambdas `corrupt(reason)` and `parseHashField(s)`. Defaults `compression = "bzip2"` if empty. Throws `corrupt(...)` if any of `havePath`, `haveNarHash`, `url`, `narSize` are missing/zero (line reset to 0 for trailer message).
- `NarInfo::to_string(const StoreDirConfig & store) const` — emits `StorePath`, `URL`, `Compression` (asserts non-empty), optional `FileHash` (asserts SHA-256), optional `FileSize`, `NarHash` (asserts SHA-256), `NarSize`, `References` (joined `shortRefs()`), optional `Deriver`, every `Sig`, optional `CA`.
- `UnkeyedNarInfo::toJSON(const StoreDirConfig * store, bool includeImpureInfo, PathInfoJsonFormat format) const` override — calls base, then if `includeImpureInfo` adds `url`, `compression`, `downloadHash` (SRI string for `PathInfoJsonFormat::V1`, raw `Hash` JSON otherwise), `downloadSize`.
- `UnkeyedNarInfo::fromJSON(const StoreDirConfig * store, const nlohmann::json & json)` static — builds `res{UnkeyedValidPathInfo::fromJSON(store, json)}`, picks format from optional `version` field (defaults to `V1`), then reads `url`, `compression`, `downloadHash` (SRI parse for V1, generic for V2), `downloadSize`.
- `nlohmann::adl_serializer<UnkeyedNarInfo>::from_json(const json & json)` — delegates to `UnkeyedNarInfo::fromJSON(nullptr, json)`.
- `nlohmann::adl_serializer<UnkeyedNarInfo>::to_json(json & json, const UnkeyedNarInfo & c)` — delegates to `c.toJSON(nullptr, true, PathInfoJsonFormat::V2)`.

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/nar-info.hh

### Namespaces
- `nix`

### Classes / structs / enums
- forward decl `struct StoreDirConfig;`.
- `struct UnkeyedNarInfo : virtual UnkeyedValidPathInfo` — fields `std::string url`, `std::string compression` (FIXME comment about `CompressionAlgo`), `std::optional<Hash> fileHash`, `uint64_t fileSize = 0`. Conversion ctor `(UnkeyedValidPathInfo info)`. Defaulted `bool operator==(const UnkeyedNarInfo &) const = default` (with comment that `<=>` is not defaulted because libc++ 16 lacks `std::optional::operator<=>`). Override `nlohmann::json toJSON(const StoreDirConfig * store, bool includeImpureInfo, PathInfoJsonFormat format) const`. Static `UnkeyedNarInfo fromJSON(const StoreDirConfig * store, const nlohmann::json & json)`.
- `struct NarInfo : ValidPathInfo, UnkeyedNarInfo` — `NarInfo() = delete`; ctors `(ValidPathInfo info)` (diamond initialiser), `(const StoreDirConfig & store, StorePath path, Hash narHash)` (delegating), `(std::string storeDir, StorePath path, Hash narHash)` (delegating), `(const StoreDirConfig & store, const std::string & s, const std::string & whence)` (parser, declared in .cc). Static `NarInfo makeFromCA(const StoreDirConfig & store, std::string_view name, ContentAddressWithReferences ca, Hash narHash)` returning `ValidPathInfo::makeFromCA(...)`. Defaulted `bool operator==(const NarInfo &) const = default`. `std::string to_string(const StoreDirConfig & store) const`.

### Functions
- (declared above; see .cc.)

### Type aliases
- (none.)

### Macros / globals
- `JSON_IMPL(nix::UnkeyedNarInfo)` (outside `namespace nix`).

---

## File: src/libstore/nar-info-disk-cache.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `struct NarInfoDiskCacheImpl : NarInfoDiskCache` — concrete cache.
  - `const int purgeInterval = 24 * 3600;` (24-hour purge cadence).
  - inner `struct Cache { int id; std::string storeDir; bool wantMassQuery; int priority; };`
  - inner `struct State { SQLite db; SQLiteStmt insertCache, queryCache, insertNAR, insertMissingNAR, queryNAR, insertRealisation, insertMissingRealisation, queryRealisation, purgeCache; std::map<std::string, Cache> caches; };` (note: `purgeCache` is declared but not actually used; purge runs ad-hoc in ctor).
  - `Sync<State> _state;`

### Functions
- `NarInfoDiskCacheImpl::NarInfoDiskCacheImpl(const Settings & settings, SQLiteSettings sqliteSettings, std::filesystem::path dbPath = getCacheDir() / "binary-cache-v8.sqlite")` — locks `_state`, calls `createDirs(dbPath.parent_path())`, opens `SQLite(dbPath, SQLite::Settings{sqliteSettings})`, calls `state->db.isCache()`, runs `schema`, prepares all SQL statements (`insertCache`, `queryCache`, `insertNAR`, `insertMissingNAR`, `queryNAR`, `insertRealisation`, `insertMissingRealisation`, `queryRealisation`), then `retrySQLite<void>` runs the `LastPurge` check: if last purge older than `purgeInterval`, deletes NAR rows whose timestamp is older than `max(settings.ttlNegative.get(), 3600U)` (negatives) or `max(settings.ttlPositive.get(), 30 * 24 * 3600U)` (positives), logs `debug("deleted %d entries ...", sqlite3_changes(state->db))`, upserts `LastPurge`.
- `NarInfoDiskCacheImpl::getCache(State & state, const std::string & uri)` — `unreachable()` if not found in `state.caches`.
- private `NarInfoDiskCacheImpl::queryCacheRaw(State & state, const std::string & uri)` — looks up `BinaryCaches` row honouring `settings.ttlMeta` (computed via `int64_t` to avoid 32-bit `time_t` promotion), populates `state.caches`.
- override `int NarInfoDiskCacheImpl::createCache(const std::string & uri, const std::string & storeDir, bool wantMassQuery, int priority)` — `retrySQLite<int>` with `SQLiteTxn`; race-checks via `queryCacheRaw`, then upserts `BinaryCaches` returning `id`.
- override `std::optional<CacheInfo> NarInfoDiskCacheImpl::upToDateCacheExists(const std::string & uri)` — wraps `queryCacheRaw` result.
- override `std::pair<Outcome, std::shared_ptr<NarInfo>> NarInfoDiskCacheImpl::lookupNarInfo(const std::string & uri, const std::string & hashPart)` — `retrySQLite`, gets cache, runs `queryNAR` against `now - settings.ttlNegative` / `now - settings.ttlPositive`; returns `{oUnknown, 0}` if absent, `{oInvalid, 0}` if `present == 0`, else builds `make_ref<NarInfo>(cache.storeDir, StorePath(hashPart + "-" + namePart), parseAnyPrefixed(narHash))`, fills `url`, `compression`, optional `fileHash`, `fileSize`, `narSize`, splits `refs` into `references`, optional `deriver`, parses `sigs`, parses `ca`.
- override `std::pair<Outcome, std::shared_ptr<Realisation>> NarInfoDiskCacheImpl::lookupRealisation(const std::string & uri, const DrvOutput & id)` — runs `queryRealisation`, returns `{oUnknown, nullptr}` if no row, `{oInvalid, nullptr}` if `outputPath` null, else constructs `Realisation{UnkeyedRealisation{.outPath = StorePath{...}, .signatures = nlohmann::json::parse(...)}, id}`. Catches `Error & e` and rethrows with `e.addTrace({}, "reading build trace key-value from the local disk cache")`.
- override `void NarInfoDiskCacheImpl::upsertNarInfo(const std::string & uri, const std::string & hashPart, std::shared_ptr<const ValidPathInfo> info)` — when `info` truthy, dynamic-casts to `const NarInfo`, binds 14 columns into `insertNAR` (using SQLiteStmt's nullable second-arg pattern for `narInfo`-only fields); else binds `insertMissingNAR` with `(cache.id, hashPart, time(nullptr))`.
- override `void NarInfoDiskCacheImpl::upsertRealisation(const std::string & uri, const Realisation & realisation)` — binds `insertRealisation` with drvPath, outputName, outPath, JSON-dumped signatures, timestamp.
- override `void NarInfoDiskCacheImpl::upsertAbsentRealisation(const std::string & uri, const DrvOutput & id)` — binds `insertMissingRealisation` with drvPath, outputName, timestamp.
- `ref<NarInfoDiskCache> NarInfoDiskCache::get(const Settings & settings, SQLiteSettings sqliteSettings)` — `static ref<NarInfoDiskCache> cache = make_ref<NarInfoDiskCacheImpl>(settings, sqliteSettings);` returns the singleton.
- `ref<NarInfoDiskCache> NarInfoDiskCache::getTest(const Settings & settings, SQLiteSettings sqliteSettings, std::filesystem::path dbPath)` — fresh `make_ref<NarInfoDiskCacheImpl>(settings, sqliteSettings, dbPath)`.

### Type aliases
- (none.)

### Macros / globals
- file-local `static const char * schema = R"sql(... create table BinaryCaches/NARs/BuildTrace/LastPurge ...)sql";` — defines DDL for the four cache tables.

---

## File: src/libstore/include/nix/store/nar-info-disk-cache.hh

### Namespaces
- `nix`

### Classes / structs / enums
- forward decls `struct SQLiteSettings;`, `struct NarInfoDiskCacheSettings;`.
- `struct NarInfoDiskCache` — abstract base with `using Settings = NarInfoDiskCacheSettings;`, `const Settings & settings;`, ctor `(const Settings & settings)`, `typedef enum { oValid, oInvalid, oUnknown } Outcome;`, virtual dtor `{}`. Inner `struct CacheInfo { int id; bool wantMassQuery; int priority; };`. Pure virtuals: `int createCache(uri, storeDir, wantMassQuery, priority)`, `std::optional<CacheInfo> upToDateCacheExists(uri)`, `std::pair<Outcome, std::shared_ptr<NarInfo>> lookupNarInfo(uri, hashPart)`, `void upsertNarInfo(uri, hashPart, info)`, `void upsertRealisation(uri, realisation)`, `void upsertAbsentRealisation(uri, id)`, `std::pair<Outcome, std::shared_ptr<Realisation>> lookupRealisation(uri, id)`. Static factories `static ref<NarInfoDiskCache> get(const Settings &, SQLiteSettings)` (process-wide singleton, ignores subsequent args per docstring with `@todo` for memo table) and `static ref<NarInfoDiskCache> getTest(const Settings &, SQLiteSettings, std::filesystem::path dbPath)`.

### Functions
- (declared above; see .cc.)

### Type aliases
- `NarInfoDiskCache::Settings = NarInfoDiskCacheSettings;` (via member `using`-declaration).

### Macros / globals
- (none.)

---

## File: src/libstore/filetransfer.cc

### Namespaces
- `nix`
- two unnamed file-scope `namespace { ... }` blocks (one for `HttpStatus` + comparator, one for the curl RAII typedefs and `curlMultiError`).

### Classes / structs / enums
- (anonymous-namespace) `enum struct HttpStatus : long { Ok = 200, Created = 201, NoContent = 204, PartialContent = 206, NotModified = 304, Unauthorized = 401, Forbidden = 403, NotFound = 404, ProxyAuthRequired = 407, RequestTimeout = 408, Gone = 410, TooManyRequests = 429, NotImplemented = 501, ServiceUnavailable = 503, HttpVersionNotSupported = 505, NetworkAuthRequired = 511 };`.
- (anonymous-namespace) `struct curlMultiError final : CloneableError<curlMultiError, Error>` — field `::CURLMcode code`; ctor `(::CURLMcode code)` initialising the base with `"unexpected curl multi error: %s", ::curl_multi_strerror(code)` and asserting `code != CURLM_OK`.
- `struct curlFileTransfer : public FileTransfer` — concrete file-transfer.
  - members `const FileTransferSettings & settings`, `curlMulti curlm`, `std::random_device rd`, `std::mt19937 mt19937`, `Sync<State> state_`, `std::thread workerThread`, `const size_t maxQueueSize`.
  - inner `struct TransferItem : public std::enable_shared_from_this<TransferItem>, public FileTransfer::Item` — fields `curlFileTransfer & fileTransfer`, `FileTransferRequest request`, `FileTransferResult result`, `std::unique_ptr<Activity> _act`, `Callback<FileTransferResult> callback`, `CURL * req = 0`, `char errbuf[CURL_ERROR_SIZE]`, `std::string statusMsg`, `uint32_t attempt = 0`, `std::chrono::steady_clock::time_point embargo`, `curlSList requestHeaders`, bit-fields `bool done:1, active:1, paused:1, enqueued:1, acceptRanges:1, hasContentEncoding:1` (all `=false`), `std::optional<uint32_t> retryAfterMs`, `curl_off_t writtenToSink = 0`, `std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now()`, `inline static const std::set<long> successfulStatuses{Ok, Created, NoContent, PartialContent, NotModified, 0}`, `LambdaSink finalSink`, `std::optional<StringSink> errorSink`, `std::exception_ptr callbackException`. Methods: `long getHTTPStatus()`, `void appendHeaders(const std::string &)`, ctor `(curlFileTransfer &, const FileTransferRequest &, Callback<FileTransferResult> &&)`, dtor (calls `curl_multi_remove_handle`/`curl_easy_cleanup` then fails any unfinished enqueued transfer with `Interrupted`), `void failEx(std::exception_ptr) noexcept`, `template<class T> void fail(T && e) noexcept`, `size_t writeCallback(...) noexcept` and static `writeCallbackWrapper`, `void appendCurrentUrl()`, `size_t headerCallback(...) noexcept` and static `headerCallbackWrapper` (status line + ETag + Content-Encoding + Accept-Ranges + Link/x-amz-meta-link `rel="immutable"` + Retry-After parsing), `Activity & act()` (lazy), `int progressCallback(curl_off_t dltotal, curl_off_t dlnow) noexcept` and static `progressCallbackWrapper`, `static int debugCallback(CURL *, curl_infotype, char *, size_t, void *) noexcept`, `size_t readCallback(char *, size_t, size_t) noexcept` and static `readCallbackWrapper`, `static int cloexec_callback(void *, curl_socket_t, curlsocktype)` (non-Windows), `size_t seekCallback(curl_off_t, int) noexcept` and static `seekCallbackWrapper`, `static int resolverCallbackWrapper(void *, void *, void *) noexcept`, `void unpause()`, `void init()`, `void finish(CURLcode)` (success path or `maybeRetry`), `void maybeRetry(FileTransfer::Error err, long httpStatus, FileTransferError && exc)` (effective settings + per-error-class base delay + `canRetry` lambda + `computeRetryDelayMs`).
  - inner `struct State` — `struct EmbargoComparator { bool operator()(const ref<TransferItem> & i1, const ref<TransferItem> & i2) { return i1->embargo > i2->embargo; } };`, `std::priority_queue<ref<TransferItem>, std::vector<ref<TransferItem>>, EmbargoComparator> incoming`, `std::vector<std::weak_ptr<Item>> unpause`; private `bool quitting = false`; public `void quit()` and `bool isQuitting()`.
- (anonymous-namespace) type aliases (see Type aliases section).

### Functions
- `std::chrono::milliseconds computeRetryDelayMs(const RetryDelayParams & p, std::mt19937 & rng)` — backoff = `clampedExponential(baseMs, attempt, ceilMs)`; floor = `retryAfterMs.value_or(0)`; non-jitter returns `max(floor, backoff)`; jitter clamps `floor + backoff` against `uint32_t` overflow then returns `uniform_int_distribution<uint32_t>(floor, ceiling)(rng)`.
- `std::optional<std::filesystem::path> FileTransferSettings::getDefaultSSLCertFile()` — non-Windows: probes `/etc/ssl/certs/ca-certificates.crt`, `/nix/var/nix/profiles/default/etc/ssl/certs/ca-bundle.crt` via `pathAccessible`; Windows: returns `std::nullopt` (relies on `CURLSSLOPT_NATIVE_CA`).
- `FileTransferSettings::FileTransferSettings()` — default ctor; honours `NIX_SSL_CERT_FILE` then `SSL_CERT_FILE` (chained via `or_else`/`and_then`/`transform`), assigns to `caFile` if non-empty.
- `static GlobalConfig::Register rFileTransferSettings(&fileTransferSettings);` (register settings).
- `(curlFileTransfer)::curlFileTransfer(const FileTransferSettings & settings)` — initialises `mt19937(rd())`, computes `maxQueueSize = settings.httpConnections.get() ? settings.httpConnections.get() * 5 : numeric_limits<size_t>::max()`; `std::call_once` runs `curl_global_init(CURL_GLOBAL_ALL)`; calls `curl_multi_init`, sets `CURLMOPT_PIPELINING=CURLPIPE_MULTIPLEX`, `CURLMOPT_MAX_TOTAL_CONNECTIONS = settings.httpConnections.get()`; spawns `workerThread` calling `workerThreadEntry()`.
- `(curlFileTransfer)::~curlFileTransfer()` — calls `stopWorkerThread()` (swallows exceptions via `ignoreExceptionInDestructor()`), then `workerThread.join()`.
- `(curlFileTransfer)::stopWorkerThread()` — `state_.lock()->quit(); wakeupMulti();`.
- `(curlFileTransfer)::wakeupMulti()` — `curl_multi_wakeup(curlm.get())`; throws `curlMultiError(ec)` on non-OK.
- `(curlFileTransfer)::workerThreadMain()` — main loop: optional non-Windows `createInterruptCallback` to call `stopWorkerThread`; on Linux, `tryUnshareFilesystem()`. Maintains `std::map<CURL *, std::shared_ptr<TransferItem>> items`. Each iteration: `curl_multi_perform` then drains `curl_multi_info_read` (`CURLMSG_DONE` -> `i->second->finish(msg->data.result); curl_multi_remove_handle; items.erase`). Computes `sleepTimeMs` from `nextWakeup`, calls `curl_multi_poll`. Pulls eligible items off the priority queue (under `state_` lock) until `items.size() + incoming.size() >= maxQueueSize` (postponed via `nextWakeup = now + 100ms`) or `embargo > now` (records `nextWakeup = item->embargo`). For each pulled item: `init`, `curl_multi_add_handle`, sets `active=true`. Drains `unpause` list and unpauses items still locked.
- `(curlFileTransfer)::workerThreadEntry()` — runs `workerThreadMain` catching `nix::Interrupted` and `std::exception` (latter logs via `printError`); if `!normalExit` calls `state_.lock()->quit()`.
- `(curlFileTransfer)::enqueueItem(ref<TransferItem> item)` — rejects uploads to schemes other than `http/https/s3` (`throw nix::Error`); checks `isQuitting`; pushes to `state->incoming`, sets `enqueued = true`, `wakeupMulti()`, returns `ItemHandle(item.get_ptr())`.
- override `ItemHandle (curlFileTransfer)::enqueueFileTransfer(const FileTransferRequest & request, Callback<FileTransferResult> callback)` — special-cases `request.uri.scheme() == "s3"` by `auto modifiedRequest = request; modifiedRequest.setupForS3();` then `enqueueItem(make_ref<TransferItem>(*this, std::move(modifiedRequest), std::move(callback)))`. Otherwise `enqueueItem(make_ref<TransferItem>(*this, request, std::move(callback)))`.
- `(curlFileTransfer)::unpauseTransfer(std::weak_ptr<Item> item)` — pushes onto `state->unpause` and `wakeupMulti`.
- override `(curlFileTransfer)::unpauseTransfer(ItemHandle handle)` — delegates to the `weak_ptr` overload.
- `ref<curlFileTransfer> makeCurlFileTransfer(const FileTransferSettings & settings = fileTransferSettings)` — `make_ref<curlFileTransfer>(settings)`.
- `ref<FileTransfer> getFileTransfer()` — locks `_fileTransfer`; if empty or quitting, replaces with a fresh `makeCurlFileTransfer()` (auto-restart-on-shutdown).
- `ref<FileTransfer> makeFileTransfer(const FileTransferSettings & settings)` — wraps `makeCurlFileTransfer`.
- `std::string FileTransferRequest::displayUri() const` — strips `user`/`password` from the parsed authority for logging; falls back to `uri.to_string()` on `BadURL`.
- `void FileTransferRequest::setupForS3()` — parses `ParsedS3URL::parse(uri.parsed())`, rewrites `uri = parsedS3.toHttpsUrl()`. Under `NIX_WITH_AWS_AUTH`: sets `awsSigV4Provider = "aws:amz:" + parsedS3.region.value_or("us-east-1") + ":s3"`, picks up `preResolvedAwsSessionToken` if `usernameAuth` already set, otherwise calls `getAwsCredentialsProvider()->maybeGetCredentials(parsedS3)` and populates `usernameAuth` + session token; if `sessionToken`, appends `("x-amz-security-token", *sessionToken)` to `headers`. When AWS auth is disabled, only logs `"S3 request without authentication (built without AWS support)"`.
- `std::future<FileTransferResult> FileTransfer::enqueueFileTransfer(const FileTransferRequest & request)` — wraps the virtual overload with a shared `std::promise`.
- `FileTransferResult FileTransfer::download(const FileTransferRequest & request)` — `enqueueFileTransfer(request).get()`.
- `FileTransferResult FileTransfer::upload(const FileTransferRequest & request)` — same body (comment: "this method is the same as download, but helps in readability").
- `FileTransferResult FileTransfer::deleteResource(const FileTransferRequest & request)` — same body.
- `void FileTransfer::download(FileTransferRequest && request, Sink & sink, std::function<void(FileTransferResult)> resultCallback)` — buffers data between worker thread and caller via `Sync<State>` (`bool quit, paused; std::exception_ptr exc; std::string data; std::condition_variable avail, request;`). Sets `request.dataCallback` to append to buffer and pause when above `fileTransferSettings.downloadBufferSize`. The callback delivered to `enqueueFileTransfer` flips `quit=true`, runs optional `resultCallback`, captures exceptions, notifies. The caller loop pulls chunks, unpauses if needed, flushes through `sink` outside the lock.
- template ctor `FileTransferError::FileTransferError(FileTransfer::Error error, std::optional<std::string> response, const Args &... args)` — initialises `CloneableError(args...)`, stores `error`/`response`, attaches `"%1%\n\nresponse body:\n\n%2%"` to `err.msg` when the response is small (`< 1024`) or contains `<html>`, otherwise sets `err.msg = HintFmt(args...)`.

### Type aliases
- (anonymous-namespace) `using curlSList = std::unique_ptr<::curl_slist, decltype([](::curl_slist * list) { ::curl_slist_free_all(list); })>;`
- (anonymous-namespace) `using curlMulti = std::unique_ptr<::CURLM, decltype([](::CURLM * multi) { ::curl_multi_cleanup(multi); })>;`

### Macros / globals
- `FileTransferSettings fileTransferSettings;`
- `static GlobalConfig::Register rFileTransferSettings(&fileTransferSettings);`
- `static auto * const _fileTransfer = new Sync<std::shared_ptr<curlFileTransfer>>;` (intentionally leaked; serves as the singleton holder for `getFileTransfer`).
- file-local `constexpr bool operator==(long lhs, HttpStatus rhs) noexcept` (anonymous-namespace; underpins comparisons like `httpStatus == HttpStatus::Ok`).

---

## File: src/libstore/include/nix/store/filetransfer.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct FileTransferSettings : Config` — settings (all `Setting<T>` members):
  - `enableHttp2{true, "http2", ...}`,
  - `userAgentSuffix{"", "user-agent-suffix", ...}`,
  - `httpConnections{25, "http-connections", ...}` with alias `{"binary-caches-parallel-connections"}`,
  - `connectTimeout{15, "connect-timeout", ...}` (with comment about `getaddrinfo` fallbacks and `CURLOPT_CONNECTTIMEOUT`),
  - `stalledDownloadTimeout{300, "stalled-download-timeout", ...}`,
  - `tries{5, "filetransfer-retry-attempts", ...}` with alias `{"download-attempts"}`,
  - `retryDelayMs{100, "filetransfer-retry-delay", ...}`,
  - `retryDelayRateLimitedMs{5000, "filetransfer-retry-delay-rate-limited", ...}`,
  - `retryMaxDelayMs{60000, "filetransfer-retry-max-delay", ...}`,
  - `retryJitter{true, "filetransfer-retry-jitter", ...}`,
  - `downloadBufferSize{1 * 1024 * 1024, "download-buffer-size", ...}`,
  - `downloadSpeed{0, "download-speed", ...}`,
  - `netrcFile{nixConfDir() / "netrc", "netrc-file", ...}` (`Setting<AbsolutePath>`),
  - `caFile{getDefaultSSLCertFile(), "ssl-cert-file", ..., {}, false}` (`Setting<std::optional<AbsolutePath>>`, last `false` suppresses default-value documentation).
  - private static `std::optional<std::filesystem::path> getDefaultSSLCertFile()`.
  - public default ctor.
- `enum struct HttpMethod { Get, Put, Head, Post, Delete };`
- `struct UsernameAuth { std::string username; std::optional<std::string> password; };`
- `enum class PauseTransfer : bool { No = false, Yes = true };`
- `struct FileTransferRequest` — fields:
  - `VerbatimURL uri`, `Headers headers`, `std::string expectedETag`, `HttpMethod method = HttpMethod::Get`, `ActivityId parentAct`, `bool decompress = true`,
  - retry overrides `std::optional<uint32_t> retryDelayMs / retryDelayRateLimitedMs / retryMaxDelayMs / retryAttempts`,
  - `std::optional<std::filesystem::path> tlsCert`, `std::optional<std::filesystem::path> tlsKey`,
  - inner `struct UploadData { UploadData(StringSource & s); UploadData(std::size_t sizeHint, RestartableSource & source); std::size_t sizeHint = 0; RestartableSource * source = nullptr; };`,
  - `std::optional<UploadData> data`, `std::string mimeType`,
  - `std::function<PauseTransfer(std::string_view data)> dataCallback`,
  - `std::optional<UsernameAuth> usernameAuth`,
  - under `NIX_WITH_AWS_AUTH` only: `std::optional<std::string> preResolvedAwsSessionToken`, private `std::optional<std::string> awsSigV4Provider` with `friend struct curlFileTransfer`.
  - ctor `(VerbatimURL uri)` (initialises `parentAct(getCurActivity())`).
  - methods `std::string displayUri() const`, `std::string verb(bool continuous = false) const` (Get/Head -> `"download(ing)"`, Put/Post -> `"upload(ing)"` with `assert(data)`, Delete -> `"delete(ing)"`, else `unreachable()`), `std::string noun() const` (Get/Head -> `"download"`, Put/Post -> `"upload"`, Delete -> `"deletion"`), `void setupForS3()`.
- `struct FileTransferResult` — `bool cached = false`, `std::string etag`, `std::vector<std::string> urls` (intentionally `string` not `ParsedURL`), `std::string data`, `uint64_t bodySize = 0`, `std::optional<std::string> immutableUrl`.
- forward decl `class Store;`.
- `struct FileTransfer` — protected `class Item {};` (empty marker base). Public inner `struct ItemHandle { std::weak_ptr<Item> item; friend struct FileTransfer; explicit ItemHandle(std::weak_ptr<Item> item); };`. Public virtual dtor. Pure virtuals `virtual ItemHandle enqueueFileTransfer(const FileTransferRequest & request, Callback<FileTransferResult> callback) = 0;` and `virtual void unpauseTransfer(ItemHandle handle) = 0;`. Helpers `std::future<FileTransferResult> enqueueFileTransfer(const FileTransferRequest &)`, `FileTransferResult download/upload/deleteResource(const FileTransferRequest &)` (synchronous), `void download(FileTransferRequest && request, Sink & sink, std::function<void(FileTransferResult)> resultCallback = {})`. Public `enum Error { NotFound, Unauthorized, Forbidden, Misc, Transient, Interrupted };`.
- `class FileTransferError final : public CloneableError<FileTransferError, Error>` — `FileTransfer::Error error;`, `std::optional<std::string> response;`. Templated ctor `template<typename... Args> FileTransferError(FileTransfer::Error error, std::optional<std::string> response, const Args &... args);`.

### Functions
- `const std::filesystem::path & nixConfDir();` (declared; defined elsewhere — used by `netrcFile` default).
- `ref<FileTransfer> getFileTransfer();`
- `ref<FileTransfer> makeFileTransfer(const FileTransferSettings & settings = fileTransferSettings);`

### Type aliases
- (none.)

### Macros / globals
- `extern FileTransferSettings fileTransferSettings;`

---

## File: src/libstore/include/nix/store/filetransfer-impl.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct RetryDelayParams { uint32_t attempt; uint32_t baseMs; uint32_t ceilMs; std::optional<uint32_t> retryAfterMs = {}; bool jitter = true; };` — with docs noting `attempt` is 1-based, `ceilMs` does not cap `retryAfterMs`, jitter defaults true.

### Functions
- `constexpr uint32_t clampedExponential(uint32_t base, uint32_t attempt, uint32_t ceil)` — `shift = min(attempt == 0 ? 0u : attempt - 1, 31u)`; widens to `uint64_t` then clamps to `uint32_t`.
- `constexpr uint32_t saturateMs(std::chrono::milliseconds d) noexcept` — clamps negative to 0 and values above `numeric_limits<uint32_t>::max()` to that maximum.
- `std::chrono::milliseconds computeRetryDelayMs(const RetryDelayParams & p, std::mt19937 & rng);` (declaration; defined in `filetransfer.cc`).

### Type aliases
- (none.)

### Macros / globals
- (none.)

---

## File: src/libstore/export-import.cc

### Namespaces
- `nix`

### Classes / structs / enums
- (none.)

### Functions
- `static void exportPath(Store & store, const StorePath & path, Sink & sink)` — file-local. Queries `store.queryPathInfo(path)`; sets up `HashSink hashSink(HashAlgorithm::SHA256)` and `TeeSink teeSink(sink, hashSink)`; calls `store.narFromPath(path, teeSink)`. Compares `hashSink.currentHash().hash` against `info->narHash`; throws `Error("hash of path '%s' has changed from '%s' to '%s'!")` unless either matches or `info->narHash` is the algorithm zero hash. Then writes `exportMagic`, the printed store path, references via `CommonProto::write(store, CommonProto::WriteConn{.to = teeSink}, info->references)`, optional deriver string (or empty), and a trailing `0`.
- `void exportPaths(Store & store, const StorePathSet & paths, Sink & sink)` — `store.topoSortPaths(paths)`, reverses, for each path emits `1` then `exportPath`, then trailing `0`.
- `StorePaths importPaths(Store & store, Source & source, CheckSigsFlag checkSigs)` — loops reading `readNum<uint64_t>(source)`; `0` ends, `1` continues, anything else throws `"input doesn't look like something created by 'nix-store --export'"`. Tees the NAR into a `StringSink saved` via `TeeSource tee{source, saved}` and parses through a `NullFileSystemObjectSink`. Validates `readInt(source) == exportMagic` (throws `"Nix archive cannot be imported; wrong format"`). Reads store path, references via `CommonProto::Serialise<StorePathSet>::read`, deriver, computes `narHash = hashString(HashAlgorithm::SHA256, saved.s)`, builds `ValidPathInfo info{path, {store, narHash}}`, sets optional `deriver`, `references`, `narSize = saved.s.size()`. Skips legacy signature: `if (readInt(source) == 1) readString(source);`. Replays the saved NAR into a `StringSource source(saved.s)` and calls `store.addToStore(info, source, NoRepair, checkSigs)`. Pushes `info.path` onto `res`.

### Type aliases
- (none.)

### Macros / globals
- (none new; uses `exportMagic` from header.)

---

## File: src/libstore/include/nix/store/export-import.hh

### Namespaces
- `nix`

### Classes / structs / enums
- (none.)

### Functions
- `void exportPaths(Store & store, const StorePathSet & paths, Sink & sink);` — declaration only.
- `StorePaths importPaths(Store & store, Source & source, CheckSigsFlag checkSigs = CheckSigs);` — declaration only.

### Type aliases
- (none.)

### Macros / globals
- `const uint32_t exportMagic = 0x4558494e;` — magic header (the bytes `NIXE`), declared as obsolete in the doc-comment.

---

## Cross-file observations

### Binary-cache class hierarchy and duplication
- `BinaryCacheStore` (abstract) is specialised by `LocalBinaryCacheStore`, `HttpBinaryCacheStore`, and `S3BinaryCacheStore`. Each provides an `upsertFile`/`fileExists`/`getFile` triple but the surrounding logic is mostly cloned: every concrete subclass duplicates the structural pattern of (a) building paths from the URL, (b) wrapping errors as a per-store `MakeError(UploadTo*)` (`UploadToHTTP`, `UploadToS3`), (c) handling `NotFound`/`Forbidden` specifically. `S3BinaryCacheStore::upsertFile` and `HttpBinaryCacheStore::upsertFile` share an identical "if `getCompressionMethod` then compress, set `Content-Encoding`, dispatch upload else dispatch upload" skeleton; the only divergence is whether the upload goes through `MultipartSink` and whether MD5 is added.
- `getCompressionMethod` lives on `HttpBinaryCacheStore`; `S3BinaryCacheStore` inherits it and `LocalBinaryCacheStore` does not compress at all. The decision tree (narinfo/ls/log) is hardcoded in one place but each caller in `S3BinaryCacheStore::upsertFile` re-implements the "compress + tag with `Content-Encoding`" sequence.
- `BinaryCacheStore::addToStoreCommon` is the single place where NAR is teed through compression+hash+listing+debug-info; it is shared by all three subclasses through `addToStore` and `addToStoreFromDump`. Worth noting that `BinaryCacheStore::addToStoreFromDump` reproduces some of `RemoteStore::addToStoreFromDump`'s ingestion-method logic, but with differing assumptions (no `Git`, requires a `StringSource` for replay).
- `RemoteFSAccessor` is constructed identically by `RemoteStore::getRemoteFSAccessor` and `BinaryCacheStore::getRemoteFSAccessor`; both pass a `requireValidPath` flag. The only difference is that `BinaryCacheStore` plumbs through `config.localNarCache` (third argument to the ctor), whereas `RemoteStore` uses the default. The corresponding `getFSAccessor(bool)` and `getFSAccessor(const StorePath &, bool)` overrides are mechanical wrappers in both stores; consolidating them in `Store` would remove ~10 lines of duplicate code.
- The "convert path back to a store hash via `MissingName`" trick appears in both `BinaryCacheStore::queryPathFromHashPart` and indirectly in `LocalBinaryCacheStore::queryAllValidPaths` (which appends `-MissingName` after stripping `.narinfo`). These are isolated workarounds for the lossy on-disk layout.

### SSH variants
- `LegacySSHStore`, `SSHStore`, and `MountedSSHStore` all share `CommonSSHStoreConfig` (settings: `sshKey`, `sshPublicHostKey`, `compress`, `remoteStore`, plus `authority`) and the `createSSHMaster(useMaster, logFD)` helper. The two SSH-store hierarchies still duplicate:
  - The `Connection` wrapper containing a `unique_ptr<SSHMaster::Connection>` and pipe wiring (`FdSink`/`FdSource`). `LegacySSHStore::Connection` carries `bool good`; `SSHStore::Connection` (in `ssh-store.cc`) overrides `closeWrite()` instead. Both define identical `openConnection` flows up to the protocol-specific handshake call.
  - `getReference` returning `Specified{scheme, authority.to_string()}` with a different scheme literal each.
  - `getProtocol` reaching into `conn->remoteVersion.toWire()` (legacy) vs. `conn->protoVersion.number.toWire()` (ssh-ng via base `RemoteStore`).
- `SSHStore::Config::openStore` and `MountedSSHStore::Config::openStore` are mechanical factory overrides; their bodies differ only in the cast to `MountedSSHStore::Config`.
- `LegacySSHStore` reimplements its own `Pool<Connection>`, `connect`, `flushBadConnections`-equivalent (`good` flag), and stat reporting (`getConnectionStats`/`getConnectionPid`) instead of leveraging `RemoteStore`. This is intentional because the protocol differs (ServeProto vs WorkerProto), but the connection/pool plumbing duplicates `RemoteStore`'s.

### UDS vs Mounted SSH parallels
- Both `UDSRemoteStore` and `MountedSSHStore` mix `RemoteStore` with `LocalFSStore` to access files through the local filesystem while dispatching commands over a remote protocol. The class-level overrides for `getFSAccessor` and `narFromPath` are nearly identical (delegate to `LocalFSStore::getFSAccessor` and `Store::narFromPath`). The only meaningful divergence is the GC root strategy: `UDSRemoteStore` overrides `addIndirectRoot` (to delegate to the daemon via `WorkerProto::Op::AddIndirectRoot`) while `MountedSSHStore` overrides `addPermRoot` (to send `WorkerProto::Op::AddPermRoot` directly).

### Connection handling and shutdown
- `RemoteStore` uses `Sync<std::set<Descriptor>> connectionFds` plus `shutdownConnections()` that calls `::shutdown(fd, SHUT_RDWR)` on every tracked descriptor; this approach is unique to the daemon-style `RemoteStore` family. `LegacySSHStore` does not maintain such a set and relies solely on the pool's good-flag invalidation. If the SSH-based stores need similar interrupt support they will need to be retrofitted with the same FD-set approach.
- Independently, `curlFileTransfer` ships its own shutdown path via `state_.lock()->quit()` plus `wakeupMulti()` (driving `curl_multi_wakeup`). The singleton `_fileTransfer` (a leaked `Sync<std::shared_ptr<curlFileTransfer>>`) auto-recreates the transfer in `getFileTransfer()` when `isQuitting()`, so callers see a transparent restart after an interrupt; this restart logic has no analogue in `RemoteStore`'s connection pool.

### Error types
- `MakeError(NoSuchBinaryCacheFile, Error)`, `MakeError(UploadToHTTP, Error)`, `MakeError(UploadToS3, Error)`, `MakeError(InvalidS3AddressingStyle, Error)`, file-local `class InvalidSSHAuthority final : CloneableError<...>`, file-local anonymous-namespace `struct curlMultiError final : CloneableError<...>` (in `filetransfer.cc`), `class AwsAuthError final : CloneableError<...>`, `class FileTransferError final : CloneableError<...>` — every implementation file defines its own bespoke error subclass instead of sharing a small handful of "transport/upload error" types. Consolidating these into a hierarchy (e.g. an `UploadError` parent for HTTP and S3 since they overlap) is straightforward.

### Settings/Config grouping
- All `*Config` types follow the same recipe: virtual `anchor()`, `name()` static, `uriSchemes()` static, `doc()` static (often `#include`-ing markdown), `getReference() override`, `openStore() override`. `RemoteStoreConfig`, `BinaryCacheStoreConfig`, `CommonSSHStoreConfig`, `HttpBinaryCacheStoreConfig`, `S3BinaryCacheStoreConfig` all add their own `Setting<…>` collections; the diamond inheritance for `MountedSSHStoreConfig` and `UDSRemoteStoreConfig` (which combine `LocalFSStoreConfig` with their respective remote configs) is the most visible duplication, requiring repeated `StoreConfig(params, FilePathType::Unix)` initialiser entries.

### File-transfer/curl logic
- `curlFileTransfer::TransferItem::finish` / `maybeRetry` contain the per-store retry policy: a 12-element `s3RetryableErrors` array (`IncompleteBody`, `InternalError`/`InternalFailure`/`InternalServerError`, `RequestExpired`, `RequestTimeout`, `RequestTimeTooSkewed`, `RequestThrottled`, `SlowDown`, `ServiceUnavailable`, `Throttling`, `ThrottledException`), HTTP status mapping (401/407 → `Unauthorized`, 403 → `Forbidden`, 404/410/`CURLE_FILE_COULDNT_READ_FILE` → `NotFound`, 4xx except 408/429 → `Misc`, 501/505/511 → `Misc`, plus a curl-error switch listing `CURLE_FAILED_INIT`/`URL_MALFORMAT`/`NOT_BUILT_IN`/`REMOTE_ACCESS_DENIED`/`FILE_COULDNT_READ_FILE`/`FUNCTION_NOT_FOUND`/`ABORTED_BY_CALLBACK`/`BAD_FUNCTION_ARGUMENT`/`INTERFACE_FAILED`/`UNKNOWN_OPTION`/`SSL_CACERT_BADFILE`/`TOO_MANY_REDIRECTS`/`WRITE_ERROR`/`UNSUPPORTED_PROTOCOL`/`BAD_CONTENT_ENCODING` as `Misc`). All retry overrides flow through `request.retryAttempts/retryDelayMs/retryDelayRateLimitedMs/retryMaxDelayMs` which are populated by `HttpBinaryCacheStore::makeRequest`'s local `propagate` lambda — that lambda repeats `setting.isOverridden() -> dest = setting.get()` four times, a small candidate for a shared helper.
- `FileTransferRequest::setupForS3` is shared by both the HTTP store path (`curlFileTransfer::enqueueFileTransfer` auto-detects `request.uri.scheme() == "s3"` and calls it on a copy) and `S3BinaryCacheStore::createMultipartUpload`/`uploadPart`/etc., where each method explicitly calls `req.setupForS3()` after `makeRequest`. There is no centralised "make S3 request" helper, leaving the multipart code path responsible for re-applying the conversion + auth on every API call.
- The retry-delay computation lives in `filetransfer-impl.hh` as `clampedExponential` + `saturateMs` + `RetryDelayParams` + `computeRetryDelayMs`; only `filetransfer.cc` defines `computeRetryDelayMs`, but the helpers are exposed in the impl header explicitly for unit tests (per the doc-comment "Not part of the public libstore API"). The S3 multipart code does not reuse this delay logic; any retries there go through `curlFileTransfer` itself.

### NAR-info disk cache vs in-memory caches
- `NarInfoDiskCache` is the persistent half (singleton via `NarInfoDiskCache::get`, with a separate `getTest` factory; both back `NarInfoDiskCacheImpl` whose `_state` is a `Sync<State>` holding the SQLite handle and prepared statements). `BinaryCacheStore::pathInfoCache` and `RemoteFSAccessor::narCache`/`narHashes` are the transient halves. There is no shared invalidation: `BinaryCacheStore::writeNarInfo` updates both `pathInfoCache` and `diskCache->upsertNarInfo`, but `RemoteFSAccessor` keeps its own per-instance `narHashes` map (`std::map<string, Hash, less<>>`) without coordination, plus a separate `NarCache` instance per accessor.
- The `BuildTrace` table (realisations) shares the same DB file as `NARs`; both have separate prepared statements (`insertRealisation`/`insertMissingRealisation`/`queryRealisation` vs. `insertNAR`/`insertMissingNAR`/`queryNAR`) but identical "missing-row sentinel + TTL split" structure. The `purgeCache` field on `State` is declared but currently unused — the periodic purge runs ad-hoc inside the ctor against `LastPurge`.

### Anchor pattern
- Every class with virtual functions in this shard defines a private out-of-line `anchor()` override (`RemoteStoreConfig`, `RemoteStore`, `UDSRemoteStoreConfig`, `UDSRemoteStore`, `BinaryCacheStoreConfig`, `BinaryCacheStore`, `LocalBinaryCacheStoreConfig`, `LocalBinaryCacheStore`, `HttpBinaryCacheStoreConfig`, `HttpBinaryCacheStore`, `SSHStoreConfig`, `SSHStore`, `MountedSSHStoreConfig`, `MountedSSHStore`, `LegacySSHStoreConfig`, `LegacySSHStore`, `CommonSSHStoreConfig`). All are empty bodies — pure boilerplate to ensure the vtable is emitted in a single TU. A macro or CRTP could eliminate this duplication entirely.

### Export/import format
- `export-import.cc` uses `exportMagic = 0x4558494e` (`NIXE`) as the per-path delimiter, prefixed by a single `1` byte (continuation) or `0` byte (terminator) at the stream level. `exportPaths` writes through this minimal framing; `importPaths` validates the magic, reads the path/refs/deriver, then ignores an optional legacy signature byte. The path's NAR is replayed through a `StringSource(saved.s)` into `Store::addToStore(info, source, NoRepair, checkSigs)` — the same generic entry point that `BinaryCacheStore::addToStore` (info+source variant) and `RemoteStore::addToStore` (info+source variant) implement, so all three families converge on this single ingestion API.
