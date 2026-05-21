# Inventory — Shard 05: libstore core (verified)

## File: src/libstore/store-api.cc

### Namespaces
- `nix` — main namespace for the file.

### Classes / structs / enums
None defined here (all types declared in headers; this file only defines members).

### Functions
- `static std::string canonStoreDir(std::string path)` (free, file-local) — canonicalize a Nix store directory path given as a string; throws `UsageError` if not absolute; returns `CanonPath(...).abs()`.
- `static std::string canonStoreDir(std::filesystem::path path)` (free, file-local) — overload for `std::filesystem::path`; throws `UsageError` if not absolute; uses `canonPath(...)`.
- `StoreConfigBase::StoreDirSetting::StoreDirSetting(Config * options, FilePathType pathType)` (ctor) — initializes `BaseSetting<std::string>` with default store dir derived from `NIX_STORE_DIR`/`NIX_STORE` env or compile-time fallback chosen by `FilePathType` (Unix vs Native, with Windows special case).
- `std::string StoreConfigBase::StoreDirSetting::parse(const std::string & str) const` (override) — canonicalize an explicit user-provided store dir string; rejects empty.
- `StoreConfigBase::StoreConfigBase(const StoreReference::Params & params, FilePathType pathType)` (ctor) — base constructor; chains `Config(params)` and constructs the `storeDir_` setting.
- `StoreConfig::StoreConfig(const Params & params, FilePathType pathType)` (ctor) — chains `StoreConfigBase` and initializes the `StoreDirConfig` reference to `storeDir_`.
- `bool StoreDirConfig::isInStore(std::string_view path) const` (member) — `isInDir(path, storeDir)`.
- `std::pair<StorePath, CanonPath> StoreDirConfig::toStorePath(std::string_view path) const` (member) — split `<storeDir>/HASH-NAME[/sub]` into `StorePath` and remaining `CanonPath`; throws if not in store.
- `std::filesystem::path Store::followLinksToStore(std::string_view _path) const` (member) — follow symlinks (cap 1024) until path is inside the store; throws `BadStorePath` if it escapes.
- `StorePath Store::followLinksToStorePath(std::string_view path) const` (member) — combines `followLinksToStore` and `toStorePath`.
- `StorePath Store::addToStore(std::string_view name, const SourcePath & path, ContentAddressMethod method, HashAlgorithm hashAlgo, const StorePathSet & references, PathFilter & filter, RepairFlag repair)` (virtual default impl) — pick `FileSerialisationMethod` from method, `dumpPath` through `sourceToSink` into `addToStoreFromDump`; warns on large paths.
- `void Store::addMultipleToStore(PathsSource && pathsToCopy, Activity & act, RepairFlag repair, CheckSigsFlag checkSigs)` (virtual default impl) — concurrent topological copy via `processGraph<StorePath>`; tracks running/done/failed counts; honors `keepGoing`.
- `ValidPathInfo Store::addToStoreSlow(std::string_view name, const SourcePath & srcPath, ContentAddressMethod method, HashAlgorithm hashAlgo, const StorePathSet & references, std::optional<Hash> expectedCAHash)` (member) — single-pass slow addition computing both NAR hash and CA hash via tee-sink graph (graphviz documented).
- `void Store::narFromPath(const StorePath & path, Sink & sink)` (virtual default impl) — dump NAR via `requireStoreObjectAccessor` + `dumpPath`.
- `StringSet Store::Config::getDefaultSystemFeatures()` (static member) — derive default system features set, gating on `Xp::CaDerivations` and `Xp::RecursiveNix`.
- `Store::Store(const Store::Config & config)` (ctor) — initializes `StoreDirConfig`, stores `config` reference, builds `pathInfoCache` (size from `config.pathInfoCacheSize`), asserts `assertLibStoreInitialized()`.
- `StoreReference StoreConfig::getReference() const` (virtual default impl) — returns `{.variant = StoreReference::Auto{}}`.
- `bool StoreConfig::getReadOnly() const` (virtual default impl) — returns `settings.readOnlyMode`.
- `bool Store::PathInfoCacheValue::isKnownNow(const NarInfoDiskCacheSettings & settings)` (member) — whether cache entry is still within positive (`didExist()`) or negative TTL window.
- `void Store::invalidatePathInfoCacheFor(const StorePath & path)` (member) — erases the in-memory cache entry.
- `std::map<std::string, std::optional<StorePath>> Store::queryStaticPartialDerivationOutputMap(const StorePath & path)` (virtual default impl) — read derivation, return `outputName→optional<StorePath>` map from `outputsAndOptPaths`.
- `std::optional<StorePath> Store::queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName)` (virtual default impl) — single-output variant; throws if output name is not declared.
- `std::map<std::string, std::optional<StorePath>> Store::queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore_)` (virtual default impl) — like static one but also runs `queryPartialDerivationOutputMapCA` when `Xp::CaDerivations` is enabled.
- `OutputPathMap Store::queryDerivationOutputMap(const StorePath & path, Store * evalStore)` (member) — wraps partial map; throws `MissingRealisation` on absent output.
- `StorePathSet Store::queryDerivationOutputs(const StorePath & path)` (virtual default impl) — collected outputs of `nix::deepQueryDerivationOutputMap`.
- `StorePathSet Store::querySubstitutablePaths(const StorePathSet & paths)` (virtual default impl) — across `getDefaultSubstituters()` matching `storeDir` and `wantMassQuery`, return paths the substituter has; honors `useSubstitutes`.
- `bool Store::isValidPath(const StorePath & storePath)` (member) — three-tier lookup (in-memory cache, disk cache, then `isValidPathUncached`); negative results may be re-cached on disk.
- `bool Store::isValidPathUncached(const StorePath & path)` (virtual default impl) — fallback that calls `queryPathInfo` and catches `InvalidPath`.
- `ref<const ValidPathInfo> Store::queryPathInfo(const StorePath & storePath)` (member, sync wrapper) — wraps the async overload via `std::promise`/`std::future`.
- `static bool goodStorePath(const StorePath & expected, const StorePath & actual)` (file-local) — checks hash part match, allowing `MissingName` placeholder for the name.
- `std::optional<std::shared_ptr<const ValidPathInfo>> Store::queryPathInfoFromClientCache(const StorePath & storePath)` (member) — only the in-memory + disk narinfo cache lookup; no upstream call.
- `void Store::queryPathInfo(const StorePath & storePath, Callback<ref<const ValidPathInfo>> callback) noexcept` (member, async) — try `queryPathInfoFromClientCache`, else `queryPathInfoUncached` and populate caches.
- `void Store::queryRealisation(const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept` (member, async) — gates on `CaDerivations`, queries disk cache, then `queryRealisationUncached`; updates disk cache for both present and absent results.
- `std::shared_ptr<const UnkeyedRealisation> Store::queryRealisation(const DrvOutput & id)` (member, sync wrapper).
- `void Store::substitutePaths(const StorePathSet & paths)` (member) — `queryMissing` then run `buildPaths` on substitutable Opaque derived paths; warns on errors.
- `StorePathSet Store::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)` (virtual default impl) — concurrent query via `ThreadPool` and `Sync<State>`, collecting valid paths.
- `std::string Store::makeValidityRegistration(const StorePathSet & paths, bool showDerivers, bool showHash)` (member) — produce text input for `nix-store --load-db`/`--register-validity`.
- `StorePathSet Store::exportReferences(const StorePathSet & storePaths, const StorePathSet & inputPaths)` (member) — closure over storePaths (must be subset of inputPaths); for derivations also includes their output paths' closures; throws `UnimplementedError` for CA derivations without resolved outputs.
- `static std::string makeCopyPathMessage(const StoreConfig & srcCfg, const StoreConfig & dstCfg, std::string_view storePath)` (file-local) — formats a "copying X from/to Y" log message recognizing local/unix shorthands.
- `void copyStorePath(Store & srcStore, Store & dstStore, const StorePath & storePath, RepairFlag repair, CheckSigsFlag checkSigs)` (free) — copy one path between stores; recomputes CA path on dst when `info->ca && info->references.empty()`; resets `ultimate`; honors `_NIX_TEST_CONCURRENT_SUBSTITUTION`.
- `std::map<StorePath, StorePath> copyPaths(Store & srcStore, Store & dstStore, const RealisedPath::Set & paths, RepairFlag, CheckSigsFlag, SubstituteFlag)` (free) — variant accepting realised paths; also registers realisations on dst (swallows `MissingExperimentalFeature` for `CaDerivations`).
- `std::map<StorePath, StorePath> copyPaths(Store & srcStore, Store & dstStore, const StorePathSet & storePaths, RepairFlag, CheckSigsFlag, SubstituteFlag)` (free) — main implementation: queries valid paths on dst, sorts missing topologically (reversed), builds `PathsSource`, calls `addMultipleToStore`.
- `void copyClosure(Store & srcStore, Store & dstStore, const RealisedPath::Set & paths, RepairFlag, CheckSigsFlag, SubstituteFlag)` (free) — close paths under references on srcStore then `copyPaths`; bails when `&srcStore == &dstStore`.
- `void copyClosure(Store & srcStore, Store & dstStore, const StorePathSet & storePaths, RepairFlag, CheckSigsFlag, SubstituteFlag)` (free) — same for plain store paths.
- `std::optional<ValidPathInfo> decodeValidPathInfo(const Store & store, std::istream & str, std::optional<HashResult> hashGiven)` (free) — parse text produced by `makeValidityRegistration`; returns `nullopt` on EOF before path.
- `Derivation Store::derivationFromPath(const StorePath & drvPath)` (member) — `ensurePath` then `readDerivation`.
- `static Derivation readDerivationCommon(Store & store, const StorePath & drvPath, bool requireValidPath)` (file-local) — load and parse a derivation via `requireStoreObjectAccessor`.
- `std::optional<StorePath> Store::getBuildDerivationPath(const StorePath & path)` (member) — for non-derivations returns the deriver from path info; for unresolved CA derivations returns the resolved drv's computed store path.
- `Derivation Store::readDerivation(const StorePath & drvPath)` (virtual default impl) — calls `readDerivationCommon` with `requireValidPath = true`.
- `Derivation Store::readInvalidDerivation(const StorePath & drvPath)` (virtual default impl) — `readDerivationCommon` with `requireValidPath = false`.
- `void Store::signPathInfo(ValidPathInfo & info)` (member) — sign with all `secretKeyFiles`.
- `void Store::signRealisation(Realisation & realisation)` (member) — same for realisations (uses `realisation.id` as key).
- `const std::filesystem::path & StoreConfig::getStateDir() const` (virtual default impl) — returns `settings.nixStateDir`.
- `const std::filesystem::path & StoreConfig::getLogDir() const` (virtual default impl) — first-call evaluates `NIX_LOG_DIR` env / Windows ProgramData / `NIX_LOG_DIR` constant.
- `const Store::Stats & Store::getStats()` (member) — refresh `pathInfoCacheSize` from current cache and return.

### Type aliases
- `using json = nlohmann::json;` (translation-unit local).
- `using PathWithInfo = std::pair<ValidPathInfo, std::unique_ptr<Source>>;` (local in `addMultipleToStore`).
- `using RealPtr = std::shared_ptr<const UnkeyedRealisation>;` (local in `queryRealisation` sync wrapper).

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-api.hh

### Namespaces
- `nix`
- `nlohmann` (via `JSON_IMPL` macro outside namespace).

### Classes / structs / enums
- `MakeError(InvalidPath, Error)` — exception for invalid store paths.
- `MakeError(Unsupported, Error)` — operation not supported by store.
- `MakeError(SubstituteGone, Error)` — substituter no longer has a path.
- `MakeError(SubstituterDisabled, Error)`.
- `MakeError(InvalidStoreReference, Error)`.
- Forward decls: `UnkeyedRealisation`, `Realisation`, `RealisedPath`, `DrvOutput`, `BasicDerivation`, `Derivation`, `SourceAccessor`, `NarInfoDiskCache`, `NarInfoDiskCacheSettings`, `Store`, `BuildResult`, `KeyedBuildResult`.
- `enum CheckSigsFlag : bool { NoCheckSigs = false, CheckSigs = true }`.
- `enum SubstituteFlag : bool { NoSubstitute = false, Substitute = true }`.
- `enum BuildMode : uint8_t { bmNormal, bmRepair, bmCheck }`.
- `enum TrustedFlag : bool { NotTrusted = false, Trusted = true }`.
- `struct MissingPaths { StorePathSet willBuild, willSubstitute, unknown; uint64_t downloadSize{0}; uint64_t narSize{0}; }` — output of `Store::queryMissing`.
- `struct StoreConfigBase : Config`:
  - Protected nested `enum struct FilePathType { Unix, Native }` — controls default-store-dir derivation strategy.
  - Public nested `class StoreDirSetting : public BaseSetting<std::string>` (friend of `StoreConfigBase`); private fields `FilePathType pathType`, private ctor `StoreDirSetting(Config * options, FilePathType pathType)`, override `std::string parse(const std::string & str) const`, inline `void operator=(const std::string & v)`.
  - Public field `StoreDirSetting storeDir_`.
  - Public ctor `StoreConfigBase(const StoreReference::Params & params, FilePathType pathType)`.
- `struct StoreConfig : public StoreConfigBase, public StoreDirConfig`:
  - Private pure virtual `void anchor() = 0` — vtable anchor to avoid dynamic_cast issues across SOs on Darwin.
  - `using Params = StoreReference::Params`.
  - Ctor `StoreConfig(const Params & params, FilePathType pathType)`; `StoreConfig() = delete`; `virtual ~StoreConfig() {}`.
  - Static `StringSet getDefaultSystemFeatures()`.
  - Static inline `std::string doc() { return ""; }` (overridden by subclasses).
  - Inline `StringMap getQueryParams() const` — collects overridden settings via `getSettings(..., overriddenOnly=true)`.
  - Static inline `std::optional<ExperimentalFeature> experimentalFeature() { return std::nullopt; }`.
  - Setting members: `Setting<int> pathInfoCacheSize{this, 65536, ...}`, `Setting<bool> isTrusted{this, false, ...}`, `Setting<int> priority{this, 0, ...}`, `Setting<bool> wantMassQuery{this, false, ...}`, `Setting<StringSet> systemFeatures{this, getDefaultSystemFeatures(), ...}`.
  - Virtual `bool getReadOnly() const`.
  - Virtual `const std::filesystem::path & getStateDir() const`.
  - Virtual `const std::filesystem::path & getLogDir() const`.
  - Pure virtual `ref<Store> openStore() const = 0`.
  - Virtual `StoreReference getReference() const`.
  - Virtual inline `std::string getHumanReadableURI() const` — defaults to `getReference().render(false)`.
- `class Store : public std::enable_shared_from_this<Store>, public StoreDirConfig`:
  - Private pure virtual `void anchor() = 0` — vtable anchor.
  - Public `using Config = StoreConfig`.
  - Public field `const Config & config`.
  - Public conversion `operator const Config &() const`.
  - Protected nested `struct PathInfoCacheValue { std::chrono::time_point<std::chrono::steady_clock> time_point = std::chrono::steady_clock::now(); std::shared_ptr<const ValidPathInfo> value; bool isKnownNow(const NarInfoDiskCacheSettings &); inline bool didExist() { return value != nullptr; } }`.
  - Protected `void invalidatePathInfoCacheFor(const StorePath & path)`.
  - Protected `ref<SharedSync<LRUCache<StorePath, PathInfoCacheValue>>> pathInfoCache`.
  - Protected `std::shared_ptr<NarInfoDiskCache> diskCache`.
  - Protected ctor `Store(const Store::Config & config)`.
  - Public virtual `void init() {}` (no-op default).
  - Public virtual `~Store() {}`.
  - Public `std::filesystem::path followLinksToStore(std::string_view path) const`.
  - Public `StorePath followLinksToStorePath(std::string_view path) const`.
  - Public `bool isValidPath(const StorePath & path)`.
  - Protected virtual `bool isValidPathUncached(const StorePath & path)`.
  - Public `void substitutePaths(const StorePathSet & paths)`.
  - Public virtual `StorePathSet queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute = NoSubstitute)`.
  - Public virtual `StorePathSet queryAllValidPaths()` — defaults to `unsupported("queryAllValidPaths")`.
  - Public `constexpr static const char * MissingName = "x"`.
  - Public `ref<const ValidPathInfo> queryPathInfo(const StorePath & path)`.
  - Public `void queryPathInfo(const StorePath & path, Callback<ref<const ValidPathInfo>> callback) noexcept`.
  - Public `std::optional<std::shared_ptr<const ValidPathInfo>> queryPathInfoFromClientCache(const StorePath & path)`.
  - Public `std::shared_ptr<const UnkeyedRealisation> queryRealisation(const DrvOutput &)`.
  - Public `void queryRealisation(const DrvOutput &, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept`.
  - Public virtual inline `bool pathInfoIsUntrusted(const ValidPathInfo &) { return true; }`.
  - Public virtual inline `bool realisationIsUntrusted(const Realisation &) { return true; }`.
  - Protected pure virtual `void queryPathInfoUncached(const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept = 0`.
  - Protected pure virtual `void queryRealisationUncached(const DrvOutput &, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept = 0`.
  - Public virtual inline `void queryReferrers(const StorePath & path, StorePathSet & referrers) { unsupported("queryReferrers"); }`.
  - Public virtual inline `StorePathSet queryValidDerivers(const StorePath & path) { return {}; }`.
  - Public virtual `StorePathSet queryDerivationOutputs(const StorePath & path)`.
  - Public virtual `std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore = nullptr)`.
  - Public virtual `std::map<std::string, std::optional<StorePath>> queryStaticPartialDerivationOutputMap(const StorePath & path)`.
  - Public virtual `std::optional<StorePath> queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName)`.
  - Public `OutputPathMap queryDerivationOutputMap(const StorePath & path, Store * evalStore = nullptr)`.
  - Public pure virtual `std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) = 0`.
  - Public virtual `StorePathSet querySubstitutablePaths(const StorePathSet & paths)`.
  - Public virtual `void querySubstitutablePathInfos(const StorePathCAMap & paths, SubstitutablePathInfos & infos)` (declaration only here).
  - Public pure virtual `void addToStore(const ValidPathInfo & info, Source & narSource, RepairFlag repair = NoRepair, CheckSigsFlag checkSigs = CheckSigs) = 0`.
  - Public `using PathsSource = std::vector<std::pair<ValidPathInfo, std::unique_ptr<Source>>>`.
  - Public virtual `void addMultipleToStore(PathsSource && pathsToCopy, Activity & act, RepairFlag repair = NoRepair, CheckSigsFlag checkSigs = CheckSigs)`.
  - Public virtual `StorePath addToStore(std::string_view name, const SourcePath & path, ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive, HashAlgorithm hashAlgo = HashAlgorithm::SHA256, const StorePathSet & references = StorePathSet(), PathFilter & filter = defaultPathFilter, RepairFlag repair = NoRepair)`.
  - Public `ValidPathInfo addToStoreSlow(std::string_view name, const SourcePath & path, ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive, HashAlgorithm hashAlgo = HashAlgorithm::SHA256, const StorePathSet & references = StorePathSet(), std::optional<Hash> expectedCAHash = {})`.
  - Public pure virtual `StorePath addToStoreFromDump(Source & dump, std::string_view name, FileSerialisationMethod dumpMethod = FileSerialisationMethod::NixArchive, ContentAddressMethod hashMethod = ContentAddressMethod::Raw::NixArchive, HashAlgorithm hashAlgo = HashAlgorithm::SHA256, const StorePathSet & references = StorePathSet(), RepairFlag repair = NoRepair) = 0`.
  - Public pure virtual `void registerDrvOutput(const Realisation & output) = 0`.
  - Public virtual inline `void registerDrvOutput(const Realisation & output, CheckSigsFlag checkSigs) { return registerDrvOutput(output); }`.
  - Public virtual `void narFromPath(const StorePath & path, Sink & sink)`.
  - Public virtual `void buildPaths(const std::vector<DerivedPath> & paths, BuildMode buildMode = bmNormal, std::shared_ptr<Store> evalStore = nullptr)` (declaration only here).
  - Public virtual `std::vector<KeyedBuildResult> buildPathsWithResults(const std::vector<DerivedPath> & paths, BuildMode buildMode = bmNormal, std::shared_ptr<Store> evalStore = nullptr)` (declaration only here).
  - Public virtual `BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode = bmNormal)` (declaration only here).
  - Public virtual `void ensurePath(const StorePath & path)` (declaration only here).
  - Public virtual inline `void addTempRoot(const StorePath & path) { debug("not creating temporary root, store doesn't support GC"); }`.
  - Public `std::string makeValidityRegistration(const StorePathSet & paths, bool showDerivers, bool showHash)`.
  - Public virtual inline `void optimiseStore() {}`.
  - Public virtual inline `bool verifyStore(bool checkContents, RepairFlag repair = NoRepair) { return false; }`.
  - Public pure virtual `ref<SourceAccessor> getFSAccessor(bool requireValidPath = true) = 0`.
  - Public pure virtual `std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath = true) = 0`.
  - Public inline `[[nodiscard]] ref<SourceAccessor> requireStoreObjectAccessor(const StorePath & path, bool requireValidPath = true)` — wraps `getFSAccessor` and throws `InvalidPath` on null.
  - Public virtual `void repairPath(const StorePath & path)` (declaration only here).
  - Public virtual inline `void addSignatures(const StorePath & storePath, const std::set<Signature> & sigs) { unsupported("addSignatures"); }`.
  - Public `void signPathInfo(ValidPathInfo & info)`, `void signRealisation(Realisation &)`.
  - Public `Derivation derivationFromPath(const StorePath & drvPath)`.
  - Public virtual `StorePath writeDerivation(const Derivation & drv, RepairFlag repair = NoRepair)` (declaration only here).
  - Public virtual `Derivation readDerivation(const StorePath & drvPath)`.
  - Public virtual `Derivation readInvalidDerivation(const StorePath & drvPath)`.
  - Public virtual `void computeFSClosure(const StorePathSet & paths, StorePathSet & out, bool flipDirection = false, bool includeOutputs = false, bool includeDerivers = false)` (declaration only here).
  - Public non-virtual overload `void computeFSClosure(const StorePath & path, StorePathSet & out, bool flipDirection = false, bool includeOutputs = false, bool includeDerivers = false)`.
  - Public virtual `MissingPaths queryMissing(const std::vector<DerivedPath> & targets)` (declaration only here).
  - Public virtual `StorePaths topoSortPaths(const StorePathSet & paths)` (declaration only here).
  - Public nested `struct Stats { std::atomic<uint64_t> narInfoRead{0}, narInfoReadAverted{0}, narInfoMissing{0}, narInfoWrite{0}, pathInfoCacheSize{0}, narRead{0}, narReadBytes{0}, narReadCompressedBytes{0}, narWrite{0}, narWriteAverted{0}, narWriteBytes{0}, narWriteCompressedBytes{0}, narWriteCompressionTimeMs{0}; }`.
  - Public `const Stats & getStats()`.
  - Public `StorePathSet exportReferences(const StorePathSet & storePaths, const StorePathSet & inputPaths)`.
  - Public `std::optional<StorePath> getBuildDerivationPath(const StorePath &)`.
  - Public inline `void clearPathInfoCache() { pathInfoCache->lock()->clear(); }`.
  - Public virtual inline `void connect() {}`.
  - Public virtual inline `unsigned int getProtocol() { return 0; }`.
  - Public pure virtual `std::optional<TrustedFlag> isTrustedClient() = 0`.
  - Public virtual inline `void setOptions() {}`.
  - Public virtual inline `std::optional<std::string> getVersion() { return {}; }`.
  - Protected `Stats stats`.
  - Protected `[[noreturn]] void unsupported(const std::string & op)` — throws `Unsupported` with URI.
- `template<> struct json_avoids_null<TrustedFlag> : std::true_type` — JSON support tag.

### Functions (free)
- `void copyStorePath(Store & srcStore, Store & dstStore, const StorePath & storePath, RepairFlag repair = NoRepair, CheckSigsFlag checkSigs = CheckSigs)`.
- `std::map<StorePath, StorePath> copyPaths(Store & srcStore, Store & dstStore, const std::set<RealisedPath> &, RepairFlag = NoRepair, CheckSigsFlag = CheckSigs, SubstituteFlag = NoSubstitute)`.
- `std::map<StorePath, StorePath> copyPaths(Store & srcStore, Store & dstStore, const StorePathSet & paths, RepairFlag = NoRepair, CheckSigsFlag = CheckSigs, SubstituteFlag = NoSubstitute)`.
- `void copyClosure(Store & srcStore, Store & dstStore, const std::set<RealisedPath> & paths, RepairFlag = NoRepair, CheckSigsFlag = CheckSigs, SubstituteFlag = NoSubstitute)`.
- `void copyClosure(Store & srcStore, Store & dstStore, const StorePathSet & paths, RepairFlag = NoRepair, CheckSigsFlag = CheckSigs, SubstituteFlag = NoSubstitute)`.
- `void removeTempRoots()` — clear temp roots file at process exit (declaration only here).
- `StorePath resolveDerivedPath(Store &, const SingleDerivedPath &, Store * evalStore = nullptr)` (declaration only here).
- `OutputPathMap resolveDerivedPath(Store &, const DerivedPath::Built &, Store * evalStore = nullptr)` (declaration only here).
- `std::optional<ValidPathInfo> decodeValidPathInfo(const Store & store, std::istream & str, std::optional<HashResult> hashGiven = std::nullopt)`.
- `const ContentAddress * getDerivationCA(const BasicDerivation & drv)` (declaration only here).

### Type aliases
- `typedef std::map<std::string, StorePath> OutputPathMap;`.
- `typedef std::map<StorePath, std::optional<ContentAddress>> StorePathCAMap;`.

### Macros / globals
- `JSON_IMPL(nix::TrustedFlag)` — outside namespace, declares `nlohmann::adl_serializer<nix::TrustedFlag>`.

## File: src/libstore/store-dir-config.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `StorePath StoreDirConfig::parseStorePath(std::string_view path) const` (member) — parse `<storeDir>/<base>` path; on Windows uses `std::filesystem::path` directly, elsewhere `canonPath`; throws `BadStorePath` if parent doesn't match `storeDir`.
- `std::optional<StorePath> StoreDirConfig::maybeParseStorePath(std::string_view path) const` (member) — non-throwing variant catching `Error`.
- `bool StoreDirConfig::isStorePath(std::string_view path) const` (member) — `(bool) maybeParseStorePath(path)`.
- `StorePathSet StoreDirConfig::parseStorePathSet(const StringSet & paths) const` (member) — bulk parse.
- `std::string StoreDirConfig::printStorePath(const StorePath & path) const` (member) — concatenation of `storeDir + "/"` and base name.
- `StringSet StoreDirConfig::printStorePathSet(const StorePathSet & paths) const` (member) — bulk render.
- `StorePath StoreDirConfig::makeStorePath(std::string_view type, std::string_view hash, std::string_view name) const` (member) — implements store-path computation per protocol spec (`type:hash:storeDir:name`, SHA-256 then compressed to 20 bytes).
- `StorePath StoreDirConfig::makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const` (member) — overload; calls the string version with `hash.to_string(HashFormat::Base16, true)`.
- `StorePath StoreDirConfig::makeOutputPath(std::string_view id, const Hash & hash, std::string_view name) const` (member) — `output:<id>` type with `outputPathName(name, id)`.
- `static std::string makeType(const StoreDirConfig & store, std::string && type, const StoreReferences & references)` (file-local) — encode references and self-flag into the type token.
- `StorePath StoreDirConfig::makeFixedOutputPath(std::string_view name, const FixedOutputInfo & info) const` (member) — handles SHA-256 NixArchive (uses `source` type), fixed-output digest scheme (`fixed:out:`), and Git ingestion validation (must be SHA-1 or SHA-256, no references).
- `StorePath StoreDirConfig::makeFixedOutputPathFromCA(std::string_view name, const ContentAddressWithReferences & ca) const` (member) — visits `TextInfo` (asserts SHA-256) / `FixedOutputInfo` variants.
- `std::pair<StorePath, Hash> StoreDirConfig::computeStorePath(std::string_view name, const SourcePath & path, ContentAddressMethod method, HashAlgorithm hashAlgo, const StorePathSet & references, PathFilter & filter) const` (member) — read-only compute store path via `hashPath` + `makeFixedOutputPathFromCA`; warns if hashed path is large.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-dir-config.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- Forward decl `struct SourcePath;`.
- `MakeError(BadStorePath, Error)`.
- `MakeError(BadStorePathName, BadStorePath)`.
- `struct StoreDirConfig`:
  - Field `const std::string & storeDir` (reference, not owned).
  - `StorePath parseStorePath(std::string_view path) const`.
  - `std::optional<StorePath> maybeParseStorePath(std::string_view path) const`.
  - `std::string printStorePath(const StorePath & path) const`.
  - `StorePathSet parseStorePathSet(const StringSet & paths) const` (deprecated todo).
  - `StringSet printStorePathSet(const StorePathSet & path) const`.
  - `bool isInStore(std::string_view path) const`.
  - `bool isStorePath(std::string_view path) const`.
  - `std::pair<StorePath, CanonPath> toStorePath(std::string_view path) const`.
  - `StorePath makeStorePath(std::string_view type, std::string_view hash, std::string_view name) const`.
  - `StorePath makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const`.
  - `StorePath makeOutputPath(std::string_view id, const Hash & hash, std::string_view name) const`.
  - `StorePath makeFixedOutputPath(std::string_view name, const FixedOutputInfo & info) const`.
  - `StorePath makeFixedOutputPathFromCA(std::string_view name, const ContentAddressWithReferences & ca) const`.
  - `std::pair<StorePath, Hash> computeStorePath(std::string_view name, const SourcePath & path, ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive, HashAlgorithm hashAlgo = HashAlgorithm::SHA256, const StorePathSet & references = {}, PathFilter & filter = defaultPathFilter) const`.

### Functions
None outside the struct.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/store-reference.cc

### Namespaces
- `nix`
- `(anonymous)` — holds `SchemeAndAuthorityWithPath`.
- `nlohmann` — `adl_serializer<StoreReference>` specialization.

### Classes / structs / enums
- `struct SchemeAndAuthorityWithPath { std::string_view scheme, authority; }` — file-local helper in anonymous namespace.

### Functions
- `static bool isNonUriPath(const std::string & spec)` (file-local) — heuristic that the input is a path (no `://`, contains a path separator).
- `std::string StoreReference::render(bool withParams) const` (member) — render the variant (`auto`/`daemon`/`local`/`scheme://authority`) with optional `?key=value` query string.
- `static std::optional<SchemeAndAuthorityWithPath> splitSchemePrefixTo(std::string_view string)` (file-local) — strip `scheme:` and optional `//` prefix, return scheme + remainder.
- `StoreReference StoreReference::parse(const std::string & uri, const StoreReference::Params & extraParams)` (static member) — build a `StoreReference` from a URI; supports `auto`, `daemon`, `local`, raw paths, IPv6 back-compat shim with optional ZoneID handling.
- `std::pair<std::string, StoreReference::Params> splitUriAndParams(const std::string & uri_)` (free) — split `?key=value` query off the URI; uses `decodeQuery(..., lenient=true)`.
- `StoreReference adl_serializer<StoreReference>::from_json(const json &)` — wraps `StoreReference::parse`.
- `void adl_serializer<StoreReference>::to_json(json &, const StoreReference &)` — wraps `ref.render()`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-reference.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct StoreReference`:
  - `using Params = StringMap`.
  - Nested `struct Auto` with default `==`/`<=>` (declared inline).
  - Nested `struct Specified { std::string scheme; std::string authority = ""; }` with default `==`/`<=>`.
  - Nested `struct Daemon : Specified` (default-constructs with `scheme = "unix"`).
  - Nested `struct Local : Specified` (default-constructs with `scheme = "local"`).
  - `typedef std::variant<Auto, Specified, Daemon, Local> Variant`.
  - Fields: `Variant variant; Params params;`.
  - Default `bool operator==`/`auto operator<=>`.
  - `std::string render(bool withParams = true) const`.
  - Inline `std::string to_string() const { return render(); }`.
  - Static `StoreReference parse(const std::string & uri, const Params & extraParams = Params{})`.
- `template<> struct json_avoids_null<StoreReference> : std::true_type`.

### Functions
- `static inline std::ostream & operator<<(std::ostream & os, const StoreReference & ref)` (free) — prints `ref.render()`.
- `std::pair<std::string, StoreReference::Params> splitUriAndParams(const std::string & uri)` (free, declaration).

### Type aliases
None outside the struct.

### Macros / globals
- `NIX_DECLARE_CONFIG_SERIALISER(StoreReference)`, `NIX_DECLARE_CONFIG_SERIALISER(std::vector<StoreReference>)`, `NIX_DECLARE_CONFIG_SERIALISER(std::set<StoreReference>)`.
- `JSON_IMPL(StoreReference)` (outside namespace).

## File: src/libstore/store-registration.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- Local struct in the `Auto` lambda inside `resolveStoreConfig`: `struct TempLocalFSStoreConfig : LocalFSStore::Config` with custom ctor and `ref<Store> openStore() const override { unreachable(); }` — used to materialize the abstract `LocalFSStoreConfig` fields for the `auto` resolution.

### Functions
- `ref<Store> openStore()` (free) — opens default store from `settings.storeUri`.
- `ref<Store> openStore(const std::string & uri, const Store::Config::Params & extraParams)` (free) — parses URI then opens.
- `ref<Store> openStore(StoreReference && storeURI)` (free) — `resolveStoreConfig(...)`, calls `openStore()` virtual, then `init()`.
- `ref<StoreConfig> resolveStoreConfig(StoreReference && storeURI)` (free) — visits the variant: `Auto` falls through `LocalStore`/`UDSRemoteStore`/Linux chroot-store decision tree; `Specified` looks up `Implementations::registered()` by scheme. Calls `experimentalFeatureSettings.require(...)` and `warnUnknownSettings()` before returning.
- `std::list<ref<Store>> getDefaultSubstituters()` (free) — singleton list built from worker's substituters list, sorted by `priority` ascending.
- `Implementations::Map & Implementations::registered()` (static member) — function-local static map.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-registration.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct StoreFactory`:
  - Field `std::string doc`.
  - Field `StringSet uriSchemes`.
  - Field `std::optional<ExperimentalFeature> experimentalFeature`.
  - Field `fun<ref<StoreConfig>(std::string_view scheme, std::string_view authorityPath, const Store::Config::Params & params)> parseConfig`.
  - Field `fun<ref<StoreConfig>()> getConfig`.
- `struct Implementations`:
  - `using Map = std::map<std::string, StoreFactory>`.
  - Static `Map & registered()`.
  - Static template `template<typename TConfig> static void add()` — synthesizes a `StoreFactory` and inserts into `registered()`. Detects which constructor `TConfig` exposes (path+params, ParsedURL+params, Authority+params, fallback scheme+authority+params).
- `template<typename TConfig> struct RegisterStoreImplementation` — calls `Implementations::add<TConfig>()` from its constructor (intended as static initializer).

### Functions
None free.

### Type aliases
None outside structs.

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-open.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `ref<StoreConfig> resolveStoreConfig(StoreReference && storeURI)`.
- `ref<Store> openStore(StoreReference && storeURI)`.
- `ref<Store> openStore(const std::string & uri, const StoreReference::Params & extraParams = StoreReference::Params())`.
- `ref<Store> openStore()` — short-hand for default store from settings.
- `std::list<ref<Store>> getDefaultSubstituters()`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/store-cast.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `template<typename T> T & require(Store & store)` (free) — `dynamic_cast<T*>(&store)`; throws `UsageError` mentioning `T::operationName` and the store URI if cast fails.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/path.cc

### Namespaces
- `nix`
- `nlohmann` — `adl_serializer<StorePath>` specialization.

### Classes / structs / enums
None defined.

### Functions
- `void checkName(std::string_view name)` (free) — enforces store path name rules: non-empty, max length (`StorePath::MaxPathLen`), no `.`/`..` followed by end or `-`, allowed character set `[0-9a-zA-Z+\-._?=]`.
- `static void checkPathName(std::string_view path, std::string_view name)` (file-local) — calls `checkName` and rethrows as `BadStorePath`.
- `StorePath::StorePath(std::string_view _baseName)` (ctor) — full base-name parser; validates length and base-32 character set on hash part (rejects `e`/`o`/`u`/`t`).
- `StorePath::StorePath(const Hash & hash, std::string_view _name)` (ctor) — build base name from `hash.to_string(HashFormat::Nix32, false) + "-" + name`.
- `bool StorePath::isDerivation() const noexcept` (member) — name ends in `.drv` (`drvExtension`).
- `void StorePath::requireDerivation() const` (member) — throws `FormatError` if not.
- `StorePath StorePath::random(std::string_view name)` (static member) — uses `Hash::random(HashAlgorithm::SHA1)`.
- `StorePath adl_serializer<StorePath>::from_json(const json & json)` — `StorePath{getString(json)}`.
- `void adl_serializer<StorePath>::to_json(json & json, const StorePath & storePath)` — `json = storePath.to_string()`.

### Type aliases
None.

### Macros / globals
- `StorePath StorePath::dummy("ffffffffffffffffffffffffffffffff-x");` — definition of static dummy path.

## File: src/libstore/include/nix/store/path.hh

### Namespaces
- `nix`
- `std` (specialization of `std::hash` for `StorePath`).

### Classes / structs / enums
- Forward decl `struct Hash;`.
- `class StorePath`:
  - Private `std::string baseName`.
  - Public `constexpr static size_t HashLen = 32;` (160 bits).
  - Public `constexpr static size_t MaxPathLen = 211;`.
  - `StorePath() = delete`.
  - Ctor `StorePath(std::string_view baseName)`.
  - Ctor `StorePath(const Hash & hash, std::string_view name)`.
  - Inline `std::string_view to_string() const noexcept` — returns view of `baseName`.
  - Default `bool operator==`/`auto operator<=>` (both `noexcept`).
  - `bool isDerivation() const noexcept`.
  - `void requireDerivation() const`.
  - Inline `std::string_view name() const` — `baseName.substr(HashLen + 1)`.
  - Inline `std::string_view hashPart() const` — `baseName.substr(0, HashLen)`.
  - Static `StorePath dummy`.
  - Static `StorePath random(std::string_view name)`.
- `template<> struct json_avoids_null<StorePath> : std::true_type`.
- `template<> struct std::hash<nix::StorePath>` — `operator()` reads first `sizeof(size_t)` bytes of base32 hash as the hash code.

### Functions
- `void checkName(std::string_view name)` (free, declaration).
- `inline std::size_t hash_value(const StorePath & path)` (free) — Boost-style hash adaptor calling `std::hash<StorePath>{}(path)`.

### Type aliases
- `typedef std::set<StorePath> StorePathSet;`.
- `typedef std::vector<StorePath> StorePaths;`.

### Macros / globals
- `constexpr std::string_view drvExtension = ".drv";`.
- `JSON_IMPL(nix::StorePath)`.

## File: src/libstore/include/nix/store/path-regex.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
None.

### Type aliases
None.

### Macros / globals
- `static constexpr std::string_view nameRegexStr` — regex pattern `(?!\.\.?(-|$))[0-9a-zA-Z\+\-\._\?=]+` for legal store-path names.

## File: src/libstore/path-info.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations.

### Classes / structs / enums
None defined.

### Functions
- `PathInfoJsonFormat parsePathInfoJsonFormat(uint64_t version)` (free) — switch on 1/2/3 returning the enum; throws `Error` otherwise.
- `UnkeyedValidPathInfo::UnkeyedValidPathInfo(const StoreDirConfig & store, Hash narHash)` (ctor) — delegates to `(string storeDir, Hash)` ctor using `store.storeDir`.
- `GENERATE_CMP_EXT(, std::weak_ordering, UnkeyedValidPathInfo, me->storeDir, me->deriver, me->narHash, me->references, me->registrationTime, me->narSize, me->ultimate, me->sigs, me->ca)` (macro expansion) — produces `==` and `<=>` on these fields (excluding `id`).
- `std::string ValidPathInfo::fingerprint(const StoreDirConfig & store) const` (member) — builds binary-cache signature payload `1;<path>;<narHash Nix32>;<narSize>;<refs csv>`; throws if `narSize == 0`.
- `void ValidPathInfo::sign(const Store & store, const Signer & signer)` (member) — single-signer overload.
- `void ValidPathInfo::sign(const Store & store, const std::vector<std::unique_ptr<Signer>> & signers)` (member) — multi-signer overload.
- `std::optional<ContentAddressWithReferences> ValidPathInfo::contentAddressWithReferences() const` (member) — splits self vs others references and packages into `TextInfo` (Text method) or `FixedOutputInfo` (Flat/NixArchive/Git/default).
- `bool ValidPathInfo::isContentAddressed(const StoreDirConfig & store) const` (member) — verifies `makeFixedOutputPathFromCA` round-trip.
- `size_t ValidPathInfo::checkSignatures(const StoreDirConfig & store, const PublicKeys & publicKeys) const` (member) — returns `maxSigs` when content addressed, else count of valid signatures.
- `bool ValidPathInfo::checkSignature(const StoreDirConfig & store, const PublicKeys & publicKeys, const Signature & sig) const` (member) — `verifyDetached(fingerprint, sig, publicKeys)`.
- `Strings ValidPathInfo::shortRefs() const` (member) — base names of references.
- `ValidPathInfo ValidPathInfo::makeFromCA(const StoreDirConfig & store, std::string_view name, ContentAddressWithReferences && ca, Hash narHash)` (static member) — builds info using `makeFixedOutputPathFromCA`; populates `ca` and `references` (handling self-reference for FixedOutputInfo).
- `nlohmann::json UnkeyedValidPathInfo::toJSON(const StoreDirConfig * store, bool includeImpureInfo, PathInfoJsonFormat format) const` (virtual member) — emits version-aware JSON: V1 uses string narHash and full path strings; V2 uses structured hashes and base-name store paths; V3 uses structured signatures.
- `UnkeyedValidPathInfo UnkeyedValidPathInfo::fromJSON(const StoreDirConfig * store, const nlohmann::json & _json)` (static member) — inverse of `toJSON`; defaults to V1 if `version` key missing.
- `PathInfoJsonFormat adl_serializer<PathInfoJsonFormat>::from_json/to_json` (nlohmann specializations).
- `UnkeyedValidPathInfo adl_serializer<UnkeyedValidPathInfo>::from_json/to_json` — calls `fromJSON(nullptr, ...)`/`toJSON(nullptr, true, V3)`.
- `ValidPathInfo adl_serializer<ValidPathInfo>::from_json/to_json` — composes UnkeyedValidPathInfo serializer plus separate `path` field.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/path-info.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- Forward decls `class Store; struct StoreDirConfig;`.
- `enum class PathInfoJsonFormat { V1 = 1, V2 = 2, V3 = 3 }` — controls JSON shape for path-info.
- `struct SubstitutablePathInfo { std::optional<StorePath> deriver; StorePathSet references; uint64_t downloadSize; uint64_t narSize; }`.
- `using SubstitutablePathInfos = std::map<StorePath, SubstitutablePathInfo>`.
- `struct UnkeyedValidPathInfo`:
  - Fields: `std::string storeDir`; `std::optional<StorePath> deriver`; `Hash narHash`; `StorePathSet references`; `time_t registrationTime = 0`; `uint64_t narSize = 0`; `uint64_t id = 0`; `bool ultimate = false`; `std::set<Signature> sigs`; `std::optional<ContentAddress> ca`.
  - Defaulted copy ctor.
  - Ctor `UnkeyedValidPathInfo(const StoreDirConfig & store, Hash narHash)`.
  - Inline ctor `UnkeyedValidPathInfo(std::string storeDir, Hash narHash)`.
  - `bool operator==(const UnkeyedValidPathInfo &) const noexcept`; `std::weak_ordering operator<=>(const UnkeyedValidPathInfo &) const noexcept`.
  - Virtual `~UnkeyedValidPathInfo() {}`.
  - Virtual `nlohmann::json toJSON(const StoreDirConfig * store, bool includeImpureInfo, PathInfoJsonFormat format) const`.
  - Static `UnkeyedValidPathInfo fromJSON(const StoreDirConfig * store, const nlohmann::json & json)`.
- `struct ValidPathInfo : virtual UnkeyedValidPathInfo`:
  - Field `StorePath path`.
  - Defaulted `bool operator==`/`auto operator<=>`.
  - `std::string fingerprint(const StoreDirConfig & store) const`.
  - `void sign(const Store & store, const Signer & signer)`.
  - `void sign(const Store & store, const std::vector<std::unique_ptr<Signer>> & signers)`.
  - `std::optional<ContentAddressWithReferences> contentAddressWithReferences() const`.
  - `bool isContentAddressed(const StoreDirConfig & store) const`.
  - Static `const size_t maxSigs = std::numeric_limits<size_t>::max()`.
  - `size_t checkSignatures(const StoreDirConfig & store, const PublicKeys & publicKeys) const`.
  - `bool checkSignature(const StoreDirConfig & store, const PublicKeys & publicKeys, const Signature & sig) const`.
  - `Strings shortRefs() const`.
  - Inline ctor `ValidPathInfo(StorePath && path, UnkeyedValidPathInfo info)`.
  - Inline ctor `ValidPathInfo(const StorePath & path, UnkeyedValidPathInfo info)` — delegates to the rvalue ctor.
  - Static `ValidPathInfo makeFromCA(const StoreDirConfig & store, std::string_view name, ContentAddressWithReferences && ca, Hash narHash)`.

### Functions
- `PathInfoJsonFormat parsePathInfoJsonFormat(uint64_t version)` (free, declaration).

### Type aliases
- `using SubstitutablePathInfos = std::map<StorePath, SubstitutablePathInfo>;`.
- `using ValidPathInfos = std::map<StorePath, ValidPathInfo>;`.

### Macros / globals
- `static_assert(std::is_move_assignable_v<ValidPathInfo>);`, `static_assert(std::is_copy_assignable_v<ValidPathInfo>);`, `static_assert(std::is_copy_constructible_v<ValidPathInfo>);`, `static_assert(std::is_move_constructible_v<ValidPathInfo>);`.
- `JSON_IMPL(nix::PathInfoJsonFormat)`, `JSON_IMPL(nix::UnkeyedValidPathInfo)`, `JSON_IMPL(nix::ValidPathInfo)`.

## File: src/libstore/path-references.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `PathRefScanSink::PathRefScanSink(StringSet && hashes, std::map<std::string, StorePath> && backMap)` (private ctor) — initializes base `RefScanSink` and the `backMap`.
- `PathRefScanSink PathRefScanSink::fromPaths(const StorePathSet & refs)` (static member) — build from a set of store paths, deriving `hashPart()` strings and the back-mapping (asserts uniqueness on insert).
- `StorePathSet PathRefScanSink::getResultPaths()` (member) — translate found hash strings back to `StorePath` via `backMap`.
- `StorePathSet scanForReferences(Sink & toTee, const std::filesystem::path & path, const StorePathSet & refs)` (free) — dump `path` through a `TeeSink` into a `PathRefScanSink` and return found paths.
- `void scanForReferencesDeep(SourceAccessor & accessor, const CanonPath & rootPath, const StorePathSet & refs, fun<void(FileRefScanResult)> callback)` (free) — recursive walker using a deducing-this lambda; per file/symlink emits `FileRefScanResult` (using a fresh sink each time so early hits don't suppress later files); recurses into directories; throws on unsupported types (char/block/socket/fifo/unknown).
- `std::map<CanonPath, StorePathSet> scanForReferencesDeep(SourceAccessor & accessor, const CanonPath & rootPath, const StorePathSet & refs)` (free) — collecting wrapper around the callback variant.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/path-references.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class PathRefScanSink : public RefScanSink`:
  - Private `std::map<std::string, StorePath> backMap`.
  - Private ctor `PathRefScanSink(StringSet && hashes, std::map<std::string, StorePath> && backMap)`.
  - Static `PathRefScanSink fromPaths(const StorePathSet & refs)`.
  - `StorePathSet getResultPaths()`.
- `struct FileRefScanResult { CanonPath filePath; StorePathSet foundRefs; }` — single-file scan result.

### Functions
- `StorePathSet scanForReferences(Sink & toTee, const std::filesystem::path & path, const StorePathSet & refs)` (declaration).
- `void scanForReferencesDeep(SourceAccessor & accessor, const CanonPath & rootPath, const StorePathSet & refs, fun<void(FileRefScanResult)> callback)` (declaration).
- `std::map<CanonPath, StorePathSet> scanForReferencesDeep(SourceAccessor & accessor, const CanonPath & rootPath, const StorePathSet & refs)` (declaration).

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/references.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `static constexpr auto refLength = StorePath::HashLen` (file-local constant) — width of a base32 hash to scan for.
- `static void search(std::string_view s, StringSet & hashes, StringSet & seen)` (file-local) — scan a buffer for any `refLength`-char base32 sequence (using `BaseNix32::lookupReverse`); when matching one of the target `hashes`, move it to `seen`.
- `void RefScanSink::operator()(std::string_view data) override` — accumulate; concatenates previous-fragment tail with leading bytes of new data to handle cross-buffer matches; updates `tail`.
- `RewritingSink::RewritingSink(const std::string & from, const std::string & to, Sink & nextSink)` (ctor) — single-rewrite ctor delegating to map ctor.
- `RewritingSink::RewritingSink(const StringMap & rewrites, Sink & nextSink)` (ctor) — main ctor; computes `maxRewriteSize` and asserts equal lengths for each `from`/`to`.
- `void RewritingSink::operator()(std::string_view data) override` — buffer/rewrite/forward via `rewriteStrings`; preserves up to `maxRewriteSize - 1` bytes for cross-call matches; advances `pos`.
- `void RewritingSink::flush()` — drain trailing buffer.
- `HashModuloSink::HashModuloSink(HashAlgorithm ha, const std::string & modulus)` (ctor) — composes hash sink with rewriting sink that zeros out the modulus pattern.
- `void HashModuloSink::operator()(std::string_view data) override` — forward to `rewritingSink`.
- `HashResult HashModuloSink::finish() override` — flush, hash positions of self-references stored in `rewritingSink.matches` (writes `|<pos>` strings), return `{hash, numBytesDigested = rewritingSink.pos}`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/references.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class RefScanSink : public Sink`:
  - Private `StringSet hashes`, `StringSet seen`, `std::string tail`.
  - Inline ctor `RefScanSink(StringSet && hashes)`.
  - Inline `StringSet & getResult() { return seen; }`.
  - `void operator()(std::string_view) override`.
- `struct RewritingSink : Sink`:
  - Fields: `const StringMap rewrites`; `std::string::size_type maxRewriteSize`; `std::string prev`; `Sink & nextSink`; `uint64_t pos = 0`; `std::vector<uint64_t> matches`.
  - Ctor `RewritingSink(const std::string & from, const std::string & to, Sink & nextSink)`.
  - Ctor `RewritingSink(const StringMap & rewrites, Sink & nextSink)`.
  - `void operator()(std::string_view) override`.
  - `void flush()`.
- `struct HashModuloSink : AbstractHashSink`:
  - Fields `HashSink hashSink`; `RewritingSink rewritingSink`.
  - Ctor `HashModuloSink(HashAlgorithm ha, const std::string & modulus)`.
  - `void operator()(std::string_view) override`.
  - `HashResult finish() override`.

### Functions
None outside structs.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/path-with-outputs.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `std::string StorePathWithOutputs::to_string(const StoreDirConfig & store) const` (member) — `<store path>` (optionally `!out1,out2,...`).
- `DerivedPath StorePathWithOutputs::toDerivedPath() const` (member) — non-empty outputs → `Built` with `Names`; empty outputs on a derivation path → `Built` with `All`; otherwise `Opaque`.
- `std::vector<DerivedPath> toDerivedPaths(const std::vector<StorePathWithOutputs> ss)` (free) — bulk conversion with `reserve`.
- `StorePathWithOutputs::ParseResult StorePathWithOutputs::tryFromDerivedPath(const DerivedPath & p)` (static member) — visits to encode a `DerivedPath` back into the legacy form: `Opaque` returns `StorePath` for derivations or `StorePathWithOutputs` for non-drvs; `Built` with `Opaque` drvPath returns `StorePathWithOutputs` (legacy: `All` → empty outputs); `Built` with `Built` drvPath returns `std::monostate{}`.
- `std::pair<std::string_view, StringSet> parsePathWithOutputs(std::string_view s)` (free) — split on `!`; outputs tokenized on `,`.
- `StorePathWithOutputs parsePathWithOutputs(const StoreDirConfig & store, std::string_view pathWithOutputs)` (free) — parse and resolve to `StorePathWithOutputs` via `parseStorePath`.
- `StorePathWithOutputs followLinksToStorePathWithOutputs(const Store & store, std::string_view pathWithOutputs)` (free) — like above but `followLinksToStorePath`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/path-with-outputs.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- Forward decls `struct StoreDirConfig;` and `class Store;` (the latter mid-file).
- `struct StorePathWithOutputs`:
  - Fields `StorePath path; StringSet outputs;`.
  - `std::string to_string(const StoreDirConfig & store) const`.
  - `DerivedPath toDerivedPath() const`.
  - Nested `typedef std::variant<StorePathWithOutputs, StorePath, std::monostate> ParseResult`.
  - Static `StorePathWithOutputs::ParseResult tryFromDerivedPath(const DerivedPath &)`.

### Functions
- `std::vector<DerivedPath> toDerivedPaths(const std::vector<StorePathWithOutputs>)`.
- `std::pair<std::string_view, StringSet> parsePathWithOutputs(std::string_view s)`.
- `StorePathWithOutputs parsePathWithOutputs(const StoreDirConfig & store, std::string_view pathWithOutputs)`.
- `StorePathWithOutputs followLinksToStorePathWithOutputs(const Store & store, std::string_view pathWithOutputs)`.

### Type aliases
None outside the struct.

### Macros / globals
None.

## File: src/libstore/content-address.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations.

### Classes / structs / enums
None defined.

### Functions
- `std::string_view makeFileIngestionPrefix(FileIngestionMethod m)` (free) — `""` (Flat, for back compat)/`"r:"` (NixArchive)/`"git:"` (Git, gated by `Xp::GitHashing`).
- `std::string_view ContentAddressMethod::render() const` (member) — `"text"` for Text, else `renderFileIngestionMethod(getFileIngestionMethod())`.
- `static ContentAddressMethod fileIngestionMethodToContentAddressMethod(FileIngestionMethod m)` (file-local, non-surjective) — Flat→Flat, NixArchive→NixArchive, Git→Git.
- `ContentAddressMethod ContentAddressMethod::parse(std::string_view m)` (static member) — `"text"` else delegate to `parseFileIngestionMethod`.
- `std::string_view ContentAddressMethod::renderPrefix() const` (member) — `"text:"` or `makeFileIngestionPrefix`.
- `ContentAddressMethod ContentAddressMethod::parsePrefix(std::string_view & m)` (static member) — strip `r:`/`git:` (gated)/`text:` and return matching enum (default `Flat`).
- `static std::string renderPrefixModern(const ContentAddressMethod & ca)` (file-local) — `"text:"` or `"fixed:" + makeFileIngestionPrefix(...)`.
- `std::string ContentAddressMethod::renderWithAlgo(HashAlgorithm ha) const` (member) — `renderPrefixModern(*this) + printHashAlgo(ha)`.
- `FileIngestionMethod ContentAddressMethod::getFileIngestionMethod() const` (member) — Flat/NixArchive/Git/Text→Flat.
- `std::string ContentAddress::render() const` (member) — `renderPrefixModern(method) + hash.to_string(HashFormat::Nix32, true)`.
- `static std::pair<ContentAddressMethod, HashAlgorithm> parseContentAddressMethodPrefix(std::string_view & rest)` (file-local) — parse `text:<algo>` or `fixed:<r?|git?>:<algo>`; throws `UsageError` on bad input.
- `ContentAddress ContentAddress::parse(std::string_view rawCa)` (static member) — parses prefix then `Hash::parseNonSRIUnprefixed`.
- `std::pair<ContentAddressMethod, HashAlgorithm> ContentAddressMethod::parseWithAlgo(std::string_view caMethod)` (static member) — appends `:` and calls private parser.
- `std::optional<ContentAddress> ContentAddress::parseOpt(std::string_view rawCaOpt)` (static member) — empty-string → `nullopt`.
- `std::string renderContentAddress(std::optional<ContentAddress> ca)` (free) — `ca->render()` or `""`.
- `std::string ContentAddress::printMethodAlgo() const` (member) — `renderPrefix() + printHashAlgo(hash.algo)`.
- `bool StoreReferences::empty() const` (member) — `!self && others.empty()`.
- `size_t StoreReferences::size() const` (member) — `(self ? 1 : 0) + others.size()`.
- `ContentAddressWithReferences ContentAddressWithReferences::withoutRefs(const ContentAddress & ca) noexcept` (static member) — wrap CA into `TextInfo`/`FixedOutputInfo` with empty refs.
- `ContentAddressWithReferences ContentAddressWithReferences::fromParts(ContentAddressMethod method, Hash hash, StoreReferences refs)` (static member) — error if `text` plus self-reference.
- `ContentAddressMethod ContentAddressWithReferences::getMethod() const` (member) — visit variant.
- `Hash ContentAddressWithReferences::getHash() const` (member) — visit variant.
- nlohmann adl_serializer specializations: `ContentAddressMethod::from_json/to_json` (string), `ContentAddress::from_json/to_json` (object with `method`/`hash`).

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/content-address.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct ContentAddressMethod`:
  - Nested `enum struct Raw { Flat, NixArchive, Git, Text }` (Git is `ExperimentalFeature::GitHashing`).
  - Field `Raw raw`.
  - Default `bool operator==`/`auto operator<=>`.
  - `MAKE_WRAPPER_CONSTRUCTOR(ContentAddressMethod)` macro.
  - Static `ContentAddressMethod parse(std::string_view rawCaMethod)`.
  - `std::string_view render() const`.
  - Static `ContentAddressMethod parsePrefix(std::string_view & m)`.
  - `std::string_view renderPrefix() const`.
  - Static `std::pair<ContentAddressMethod, HashAlgorithm> parseWithAlgo(std::string_view rawCaMethod)`.
  - `std::string renderWithAlgo(HashAlgorithm ha) const`.
  - `FileIngestionMethod getFileIngestionMethod() const`.
- `struct ContentAddress`:
  - Field `ContentAddressMethod method`.
  - Field `Hash hash`.
  - Default `bool operator==`/`auto operator<=>`.
  - `std::string render() const`.
  - Static `ContentAddress parse(std::string_view rawCa)`.
  - Static `std::optional<ContentAddress> parseOpt(std::string_view rawCaOpt)`.
  - `std::string printMethodAlgo() const`.
- `struct StoreReferences`:
  - Field `StorePathSet others`.
  - Field `bool self = false`.
  - `bool empty() const`; `size_t size() const`.
  - Default `bool operator==`. (`<=>` commented out — libc++16 limitation.)
- `struct TextInfo`:
  - Field `Hash hash`.
  - Field `StorePathSet references` (no self-references).
  - Default `bool operator==`. (`<=>` commented out.)
- `struct FixedOutputInfo`:
  - Field `FileIngestionMethod method`.
  - Field `Hash hash`.
  - Field `StoreReferences references`.
  - Default `bool operator==`. (`<=>` commented out.)
- `struct ContentAddressWithReferences`:
  - `typedef std::variant<TextInfo, FixedOutputInfo> Raw`.
  - Field `Raw raw`.
  - Default `bool operator==`. (`<=>` commented out.)
  - `MAKE_WRAPPER_CONSTRUCTOR(ContentAddressWithReferences)` macro.
  - Static `ContentAddressWithReferences withoutRefs(const ContentAddress &) noexcept`.
  - Static `ContentAddressWithReferences fromParts(ContentAddressMethod method, Hash hash, StoreReferences refs)`.
  - `ContentAddressMethod getMethod() const`.
  - `Hash getHash() const`.
- `template<> struct json_avoids_null<ContentAddressMethod> : std::true_type`.
- `template<> struct json_avoids_null<ContentAddress> : std::true_type`.

### Functions
- `std::string_view makeFileIngestionPrefix(FileIngestionMethod m)` (declaration).
- `std::string renderContentAddress(std::optional<ContentAddress> ca)` (declaration).

### Type aliases
None outside structs.

### Macros / globals
- `JSON_IMPL(nix::ContentAddressMethod)`, `JSON_IMPL(nix::ContentAddress)` (outside namespace).

## File: src/libstore/derived-path.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations.

### Classes / structs / enums
None defined.

### Functions
- `GENERATE_CMP_EXT(, std::strong_ordering, SingleDerivedPathBuilt, *me->drvPath, me->output)` (macro expansion) — produces `==` and `<=>` (deref `drvPath` to avoid `ref` ptr equality).
- `GENERATE_EQUAL(, DerivedPathBuilt::, DerivedPathBuilt, *me->drvPath, me->outputs)` and `GENERATE_ONE_CMP(, bool, DerivedPathBuilt::, <, DerivedPathBuilt, *me->drvPath, me->outputs)` (macro expansions) — `==` and `<` only because libc++16 lacks `std::set::operator<=>`.
- `std::string DerivedPath::Opaque::to_string(const StoreDirConfig & store) const` (member) — `store.printStorePath(path)`.
- `std::string SingleDerivedPath::Built::to_string(const StoreDirConfig & store) const` (member) — `<drvPath>^<output>`.
- `std::string SingleDerivedPath::Built::to_string_legacy(const StoreDirConfig & store) const` (member) — `<drvPath>!<output>`.
- `std::string DerivedPath::Built::to_string(const StoreDirConfig & store) const` (member) — `<drvPath>^<outputs>`.
- `std::string DerivedPath::Built::to_string_legacy(const StoreDirConfig & store) const` (member) — `<drvPath legacy>!<outputs>`.
- `std::string SingleDerivedPath::to_string(const StoreDirConfig & store) const` (member) — visits raw and dispatches.
- `std::string DerivedPath::to_string(const StoreDirConfig & store) const` (member) — visits raw and dispatches.
- `std::string SingleDerivedPath::to_string_legacy(const StoreDirConfig & store) const` (member) — visits with `Built::to_string_legacy` for Built and `Opaque::to_string` for Opaque (no legacy form).
- `std::string DerivedPath::to_string_legacy(const StoreDirConfig & store) const` (member) — same visit pattern.
- `DerivedPath::Opaque DerivedPath::Opaque::parse(const StoreDirConfig & store, std::string_view s)` (static member) — wraps `parseStorePath`.
- `void drvRequireExperiment(const SingleDerivedPath & drv, const ExperimentalFeatureSettings & xpSettings)` (free) — gates `Built` paths on `Xp::DynamicDerivations` (no-op for `Opaque`).
- `SingleDerivedPath::Built SingleDerivedPath::Built::parse(const StoreDirConfig & store, ref<const SingleDerivedPath> drv, OutputNameView output, const ExperimentalFeatureSettings & xpSettings)` (static member).
- `DerivedPath::Built DerivedPath::Built::parse(const StoreDirConfig & store, ref<const SingleDerivedPath> drv, OutputNameView outputsS, const ExperimentalFeatureSettings & xpSettings)` (static member) — uses `OutputsSpec::parse`.
- `static SingleDerivedPath parseWithSingle(const StoreDirConfig & store, std::string_view s, std::string_view separator, const ExperimentalFeatureSettings & xpSettings)` (file-local) — recursive parse on the rightmost separator.
- `SingleDerivedPath SingleDerivedPath::parse(const StoreDirConfig & store, std::string_view s, const ExperimentalFeatureSettings & xpSettings)` (static member) — separator `^`.
- `SingleDerivedPath SingleDerivedPath::parseLegacy(const StoreDirConfig & store, std::string_view s, const ExperimentalFeatureSettings & xpSettings)` (static member) — separator `!`.
- `static DerivedPath parseWith(const StoreDirConfig & store, std::string_view s, std::string_view separator, const ExperimentalFeatureSettings & xpSettings)` (file-local) — non-single variant.
- `DerivedPath DerivedPath::parse(const StoreDirConfig & store, std::string_view s, const ExperimentalFeatureSettings & xpSettings)` (static member) — separator `^`.
- `DerivedPath DerivedPath::parseLegacy(const StoreDirConfig & store, std::string_view s, const ExperimentalFeatureSettings & xpSettings)` (static member) — separator `!`.
- `DerivedPath DerivedPath::fromSingle(const SingleDerivedPath & req)` (static member) — promotes `Opaque` directly; promotes single-output `Names{output}` for Built.
- `const StorePath & SingleDerivedPath::Built::getBaseStorePath() const` (member) — `drvPath->getBaseStorePath()`.
- `const StorePath & DerivedPath::Built::getBaseStorePath() const` (member) — `drvPath->getBaseStorePath()`.
- `template<typename DP> static inline const StorePath & getBaseStorePath_(const DP & derivedPath)` (file-local helper) — visit either `Built` (recurse via drvPath) or `Opaque` (return path).
- `const StorePath & SingleDerivedPath::getBaseStorePath() const` (member).
- `const StorePath & DerivedPath::getBaseStorePath() const` (member).
- nlohmann adl_serializer specializations: `to_json`/`from_json` for `SingleDerivedPath::Opaque` (string), `SingleDerivedPath::Built` (object), `DerivedPath::Built` (object), `SingleDerivedPath` (string-or-object), `DerivedPath` (string-or-object). Built/SDP variants take an `ExperimentalFeatureSettings` parameter.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/derived-path.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- Forward decl `struct StoreDirConfig;`.
- `struct DerivedPathOpaque`:
  - Field `StorePath path`.
  - `std::string to_string(const StoreDirConfig & store) const`.
  - Static `DerivedPathOpaque parse(const StoreDirConfig & store, std::string_view)`.
  - Default `bool operator==`/`auto operator<=>`.
- Forward decl `struct SingleDerivedPath;`.
- `struct SingleDerivedPathBuilt`:
  - Field `ref<const SingleDerivedPath> drvPath`.
  - Field `OutputName output`.
  - `const StorePath & getBaseStorePath() const`.
  - `std::string to_string(const StoreDirConfig &) const` (`^` separator).
  - `std::string to_string_legacy(const StoreDirConfig &) const` (`!` separator).
  - Static `SingleDerivedPathBuilt parse(const StoreDirConfig & store, ref<const SingleDerivedPath> drvPath, OutputNameView outputs, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)`.
  - `bool operator==(const SingleDerivedPathBuilt &) const noexcept`; `std::strong_ordering operator<=>(const SingleDerivedPathBuilt &) const noexcept` (declared, defined via `GENERATE_CMP_EXT` macro in the .cc).
- `using _SingleDerivedPathRaw = std::variant<DerivedPathOpaque, SingleDerivedPathBuilt>;`.
- `struct SingleDerivedPath : _SingleDerivedPathRaw`:
  - `using Raw = _SingleDerivedPathRaw`; `using Raw::Raw`.
  - `using Opaque = DerivedPathOpaque`; `using Built = SingleDerivedPathBuilt`.
  - Inline `const Raw & raw() const`.
  - Default `bool operator==`/`auto operator<=>`.
  - `const StorePath & getBaseStorePath() const`.
  - `std::string to_string(const StoreDirConfig &) const`.
  - `std::string to_string_legacy(const StoreDirConfig &) const`.
  - Static `SingleDerivedPath parse(const StoreDirConfig &, std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)` (`^`).
  - Static `SingleDerivedPath parseLegacy(const StoreDirConfig &, std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)` (`!`).
- Free `static inline ref<SingleDerivedPath> makeConstantStorePathRef(StorePath drvPath)`.
- `struct DerivedPathBuilt`:
  - Field `ref<const SingleDerivedPath> drvPath`.
  - Field `OutputsSpec outputs`.
  - `const StorePath & getBaseStorePath() const`.
  - `std::string to_string(const StoreDirConfig &) const`/`to_string_legacy(...) const`.
  - Static `DerivedPathBuilt parse(const StoreDirConfig &, ref<const SingleDerivedPath>, std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`.
  - `bool operator==(const DerivedPathBuilt &) const noexcept`.
  - `bool operator<(const DerivedPathBuilt &) const noexcept` (no `<=>` due to libc++16).
- `using _DerivedPathRaw = std::variant<DerivedPathOpaque, DerivedPathBuilt>;`.
- `struct DerivedPath : _DerivedPathRaw`:
  - `using Raw = _DerivedPathRaw`; `using Raw::Raw`.
  - `using Opaque = DerivedPathOpaque`; `using Built = DerivedPathBuilt`.
  - Inline `const Raw & raw() const`.
  - `const StorePath & getBaseStorePath() const`.
  - `std::string to_string(const StoreDirConfig &) const`/`to_string_legacy(...) const`.
  - Statics `parse(...)` (`^`), `parseLegacy(...)` (`!`), `fromSingle(const SingleDerivedPath &)`.

### Functions
- `void drvRequireExperiment(const SingleDerivedPath & drv, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)` (free).

### Type aliases
- `typedef std::vector<DerivedPath> DerivedPaths;`.

### Macros / globals
- `JSON_IMPL(nix::SingleDerivedPath::Opaque)`, `JSON_IMPL_WITH_XP_FEATURES(nix::SingleDerivedPath::Built)`, `JSON_IMPL_WITH_XP_FEATURES(nix::SingleDerivedPath)`, `JSON_IMPL_WITH_XP_FEATURES(nix::DerivedPath::Built)`, `JSON_IMPL_WITH_XP_FEATURES(nix::DerivedPath)`.

## File: src/libstore/derived-path-map.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `template<typename V> typename DerivedPathMap<V>::ChildNode & DerivedPathMap<V>::ensureSlot(const SingleDerivedPath & k)` (member) — uses internal `fun<ChildNode &(const SingleDerivedPath &)> initIter` to recursively descend the drvPath chain, creating `map`/`childMap` entries as needed.
- `template<typename V> typename DerivedPathMap<V>::ChildNode * DerivedPathMap<V>::findSlot(const SingleDerivedPath & k)` (member) — non-creating variant returning `nullptr` if missing.
- `template<typename V> void DerivedPathMap<V>::removeSlot(const SingleDerivedPath & k, fun<bool(ChildNode &)> callback)` (member) — recursive removal using deducing-this lambda; prunes empty ancestors when both `value` is empty and `childMap` is empty.

### Type aliases
None.

### Macros / globals
- `template<> bool DerivedPathMap<StringSet>::ChildNode::operator==(const DerivedPathMap<StringSet>::ChildNode &) const noexcept = default;` — explicit specialization defaulted.
- `template struct DerivedPathMap<StringSet>::ChildNode;` — explicit instantiation.
- `template struct DerivedPathMap<StringSet>;` — explicit instantiation.
- `template struct DerivedPathMap<std::map<OutputsSpec, std::weak_ptr<DerivationTrampolineGoal>>>;` — explicit instantiation (requires `derivation-trampoline-goal.hh`).

## File: src/libstore/include/nix/store/derived-path-map.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `template<typename V> struct DerivedPathMap`:
  - Nested `struct ChildNode { V value; using Map = std::map<OutputName, ChildNode>; Map childMap; bool operator==(const ChildNode &) const noexcept; }` (the `<=>` line is commented out — libc++16 limitation).
  - `using Map = std::map<StorePath, ChildNode>; Map map;`.
  - Default `bool operator==`.
  - Member `ChildNode & ensureSlot(const SingleDerivedPath & k)`.
  - Member `ChildNode * findSlot(const SingleDerivedPath & k)`.
  - Member `void removeSlot(const SingleDerivedPath & k, fun<bool(ChildNode &)> callback)`.
- `template<> bool DerivedPathMap<StringSet>::ChildNode::operator==(const DerivedPathMap<StringSet>::ChildNode &) const noexcept;` — explicit specialization declaration.
- `extern template struct DerivedPathMap<StringSet>::ChildNode;`, `extern template struct DerivedPathMap<StringSet>;` — extern instantiation declarations.

### Functions
None.

### Type aliases
None outside the struct.

### Macros / globals
None.

## File: src/libstore/downstream-placeholder.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer template specializations and explicit instantiations.

### Classes / structs / enums
None defined.

### Functions
- `std::string DownstreamPlaceholder::render() const` (member) — `"/" + hash.to_string(HashFormat::Nix32, false)`.
- `DownstreamPlaceholder DownstreamPlaceholder::unknownCaOutput(const StorePath & drvPath, OutputNameView outputName, const ExperimentalFeatureSettings & xpSettings)` (static member) — gated on `Xp::CaDerivations`; SHA-256 of `nix-upstream-output:<drv hashPart>:<outputPathName(drvName, outputName)>` (drvName = drv path name minus `.drv`).
- `DownstreamPlaceholder DownstreamPlaceholder::unknownDerivation(const DownstreamPlaceholder & placeholder, OutputNameView outputName, const ExperimentalFeatureSettings & xpSettings)` (static member) — gated on `Xp::DynamicDerivations`; SHA-256 of `nix-computed-output:<compressed nix32>:<output>` where the input hash is compressed to 20 bytes.
- `DownstreamPlaceholder DownstreamPlaceholder::fromSingleDerivedPathBuilt(const SingleDerivedPath::Built & b, const ExperimentalFeatureSettings & xpSettings)` (static member) — recursive case: `Opaque` → `unknownCaOutput`; `Built` → recurse and `unknownDerivation`.
- `template<typename Item> DrvRef<Item> adl_serializer<DrvRef<Item>>::from_json(const json & json)` — object with `{"drvPath":"self","output":<x>}` decodes as `OutputName`, else delegates to `adl_serializer<Item>::from_json`.
- `template<typename Item> void adl_serializer<DrvRef<Item>>::to_json(json & json, const DrvRef<Item> & ref)` — symmetric.
- Explicit instantiations: `template struct adl_serializer<nix::DrvRef<StorePath>>;`, `template struct adl_serializer<nix::DrvRef<SingleDerivedPath>>;`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/downstream-placeholder.hh

### Namespaces
- `nix`
- `nlohmann`.

### Classes / structs / enums
- `template<typename Input> using DrvRef = std::variant<OutputName, Input>` — placeholder reference type.
- `class DownstreamPlaceholder`:
  - Private field `Hash hash`.
  - Private inline ctor `DownstreamPlaceholder(Hash hash)`.
  - Public `std::string render() const`.
  - Static `DownstreamPlaceholder unknownCaOutput(const StorePath & drvPath, OutputNameView outputName, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)`.
  - Static `DownstreamPlaceholder unknownDerivation(const DownstreamPlaceholder & drvPlaceholder, OutputNameView outputName, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)`.
  - Static `DownstreamPlaceholder fromSingleDerivedPathBuilt(const SingleDerivedPath::Built & built, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)`.
- `template<typename Item> struct adl_serializer<nix::DrvRef<Item>>` — declares static `from_json` and `to_json`.

### Functions
None outside types.

### Type aliases
- `template<typename Input> using DrvRef = std::variant<OutputName, Input>` (in `nix::`).

### Macros / globals
- `extern template struct adl_serializer<nix::DrvRef<nix::StorePath>>;`, `extern template struct adl_serializer<nix::DrvRef<nix::SingleDerivedPath>>;` (in `nlohmann`).

## File: src/libstore/outputs-spec.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations under `#ifndef DOXYGEN_SKIP`.

### Classes / structs / enums
None defined.

### Functions
- `bool OutputsSpec::contains(const std::string & outputName) const` (member) — visit `All` (always true) / `Names` (set membership).
- `std::optional<OutputsSpec> OutputsSpec::parseOpt(std::string_view s)` (static member) — non-throwing; catches `BadStorePathName`.
- `OutputsSpec OutputsSpec::parse(std::string_view s)` (static member) — `"*"` → `All`; else comma-separated → `Names` (each name validated with `checkName`).
- `std::optional<std::pair<std::string_view, ExtendedOutputsSpec>> ExtendedOutputsSpec::parseOpt(std::string_view s)` (static member) — split on rightmost `^`; `Default` if no `^`.
- `std::pair<std::string_view, ExtendedOutputsSpec> ExtendedOutputsSpec::parse(std::string_view s)` (static member) — throws `Error` on failure.
- `std::string OutputsSpec::to_string() const` (member) — `"*"` for `All`, comma-separated for `Names`.
- `std::string ExtendedOutputsSpec::to_string() const` (member) — `""` for `Default`, `"^" + spec` for `Explicit`.
- `OutputsSpec OutputsSpec::union_(const OutputsSpec & that) const` (member) — set-union (`All` absorbing).
- `bool OutputsSpec::isSubsetOf(const OutputsSpec & that) const` (member) — visit-based subset check.
- nlohmann adl_serializer specializations for `OutputsSpec` (`from_json`/`to_json` using JSON arrays of strings, `["*"]` for `All`) and `ExtendedOutputsSpec` (`null` ↔ `Default`).

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/outputs-spec.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `typedef std::string OutputName;`.
- `typedef std::string_view OutputNameView;`.
- `struct OutputsSpec`:
  - Nested `struct Names : std::set<OutputName, std::less<>>` with private `using BaseType = std::set<OutputName, std::less<>>`; public `using BaseType::BaseType`; manual `Names(const BaseType &)` and `Names(BaseType &&)` (both assert non-empty); `Names() = delete`.
  - Nested `struct All : std::monostate {}`.
  - `typedef std::variant<All, Names> Raw`.
  - Field `Raw raw`.
  - Default `bool operator==`.
  - Manual `bool operator<(const OutputsSpec & other) const` (libc++16 limitation).
  - `MAKE_WRAPPER_CONSTRUCTOR(OutputsSpec)`.
  - `OutputsSpec() = delete`.
  - `bool contains(const OutputName & output) const`.
  - `OutputsSpec union_(const OutputsSpec & that) const`.
  - `bool isSubsetOf(const OutputsSpec & outputs) const`.
  - Static `OutputsSpec parse(std::string_view s)`.
  - Static `std::optional<OutputsSpec> parseOpt(std::string_view s)`.
  - `std::string to_string() const`.
- `struct ExtendedOutputsSpec`:
  - Nested `struct Default : std::monostate {}`.
  - `using Explicit = OutputsSpec`.
  - `typedef std::variant<Default, Explicit> Raw`.
  - Field `Raw raw`.
  - Default `bool operator==`.
  - `bool operator<(const ExtendedOutputsSpec &) const` (declared only).
  - `MAKE_WRAPPER_CONSTRUCTOR(ExtendedOutputsSpec)`.
  - `ExtendedOutputsSpec() = delete`.
  - Static `std::pair<std::string_view, ExtendedOutputsSpec> parse(std::string_view s)`.
  - Static `std::optional<std::pair<std::string_view, ExtendedOutputsSpec>> parseOpt(std::string_view s)`.
  - `std::string to_string() const`.

### Functions
None.

### Type aliases
- `typedef std::string OutputName;`.
- `typedef std::string_view OutputNameView;`.

### Macros / globals
- `JSON_IMPL(OutputsSpec)`, `JSON_IMPL(ExtendedOutputsSpec)`.

## File: src/libstore/outputs-query.cc

### Namespaces
- `nix`
- `(anonymous)` — internal cache types and helper functions.

### Classes / structs / enums
None.

### Functions
- File-local types `using ResolveCache = boost::unordered_flat_map<StorePath, std::pair<Derivation, StorePath>>;` and `using RealisationCache = boost::unordered_flat_map<DrvOutput, std::optional<StorePath>>;` (anonymous-namespace).
- `static std::optional<StorePath> deepQueryPartialDerivationOutputImpl(Store &, const StorePath & drvPath, const std::string & outputName, Store * evalStore_, QueryRealisationFun & queryRealisation, ResolveCache & cache, RealisationCache & resCache)` (anon-ns, forward decl + later definition) — recursive lookup with caches; falls through to CA path if `staticResult` is empty and `Xp::CaDerivations` is on; uses `resCache` to avoid duplicate `queryRealisation` calls.
- `static std::optional<StorePath> resolveSingleDerivedPath(Store &, const SingleDerivedPath &, Store * evalStore_, QueryRealisationFun & queryRealisation, ResolveCache & cache, RealisationCache & resCache)` (anon-ns) — visits `Opaque`/`Built`; recurses on built drvPath then queries the inner output.
- `static std::pair<Derivation, StorePath> resolveDerivation(Store &, const StorePath & drvPath, Store * evalStore_, QueryRealisationFun & queryRealisation, ResolveCache & cache, RealisationCache & resCache)` (anon-ns) — calls `tryResolve` with custom realisation callback; computes resolved drv's store path; memoizes in `cache`.
- `void queryPartialDerivationOutputMapCA(Store & store, const StorePath & drvPath, const BasicDerivation & drv, std::map<std::string, std::optional<StorePath>> & outputs, QueryRealisationFun queryRealisation, RealisationCache & resCache)` (file-local overload) — fills `outputs` map by querying realisations (per-output cache lookup); defaults `queryRealisation` to `store.queryRealisation` if empty.
- `void queryPartialDerivationOutputMapCA(Store & store, const StorePath & drvPath, const BasicDerivation & drv, std::map<std::string, std::optional<StorePath>> & outputs, QueryRealisationFun queryRealisation)` (free) — wraps cached variant with a fresh `RealisationCache`.
- `std::map<std::string, std::optional<StorePath>> deepQueryPartialDerivationOutputMap(Store & store, const StorePath & drvPath, Store * evalStore_, QueryRealisationFun queryRealisation)` (free) — initializes default callback, calls static map then CA fixup if `Xp::CaDerivations`; uses `resolveDerivation` to resolve drv before CA fixup.
- `OutputPathMap deepQueryDerivationOutputMap(Store & store, const StorePath & drvPath, Store * evalStore, QueryRealisationFun queryRealisation)` (free) — like above; throws `MissingRealisation` for absent outputs.
- `std::optional<StorePath> deepQueryPartialDerivationOutput(Store & store, const StorePath & drvPath, const std::string & outputName, Store * evalStore_, QueryRealisationFun queryRealisation)` (free) — single-output version; instantiates fresh caches.

### Type aliases
- File-local `ResolveCache`, `RealisationCache` (above).

### Macros / globals
None.

## File: src/libstore/include/nix/store/outputs-query.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `void queryPartialDerivationOutputMapCA(Store & store, const StorePath & drvPath, const BasicDerivation & drv, std::map<std::string, std::optional<StorePath>> & outputs, QueryRealisationFun queryRealisation = {})`.
- `std::optional<StorePath> deepQueryPartialDerivationOutput(Store & store, const StorePath & drvPath, const std::string & outputName, Store * evalStore = nullptr, QueryRealisationFun queryRealisation = {})`.
- `std::map<std::string, std::optional<StorePath>> deepQueryPartialDerivationOutputMap(Store & store, const StorePath & drvPath, Store * evalStore = nullptr, QueryRealisationFun queryRealisation = {})`.
- `OutputPathMap deepQueryDerivationOutputMap(Store & store, const StorePath & drvPath, Store * evalStore = nullptr, QueryRealisationFun queryRealisation = {})`.

### Type aliases
- `using QueryRealisationFun = std::function<std::shared_ptr<const UnkeyedRealisation>(const DrvOutput &)>;`.

### Macros / globals
None.

## File: src/libstore/realisation.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations.

### Classes / structs / enums
- `MakeError(InvalidDerivationOutputId, Error)` — exception for malformed `DrvOutput` strings.

### Functions
- `DrvOutput DrvOutput::parse(const StoreDirConfig & store, std::string_view s)` (static member) — split on rightmost `^` then `parseStorePath`; throws `InvalidDerivationOutputId` if `^` missing.
- `std::string DrvOutput::render(const StoreDirConfig & store) const` (member) — `<full path>^<outputName>`.
- `std::string DrvOutput::to_string() const` (member) — `<base name>^<outputName>` (no store dir).
- `std::string UnkeyedRealisation::fingerprint(const DrvOutput & key) const` (member) — JSON-serializes a `Realisation{*this, key}`, drops `signatures` from the `value` object, returns `dump()`.
- `Signature UnkeyedRealisation::sign(const DrvOutput & key, const Signer & signer) const` (member) — returns detached signature of fingerprint (does not modify).
- `void UnkeyedRealisation::sign(const DrvOutput & key, const Signer & signer)` (member, non-const overload) — calls const overload via `std::as_const(*this)` and inserts new signature into `signatures`.
- `bool UnkeyedRealisation::checkSignature(const DrvOutput & key, const PublicKeys & publicKeys, const Signature & sig) const` (member) — verifies a single signature.
- `size_t UnkeyedRealisation::checkSignatures(const DrvOutput & key, const PublicKeys & publicKeys) const` (member) — count of valid signatures.
- `const StorePath & RealisedPath::path() const &` (member) — visit either variant's `getPath()`.
- `MissingRealisation::MissingRealisation(const StoreDirConfig & store, const StorePath & drvPath, const OutputName & outputName)` (ctor) — base ctor; message `"cannot operate on output '%s' of the unbuilt derivation '%s'"`.
- `MissingRealisation::MissingRealisation(const StoreDirConfig & store, const SingleDerivedPath & drvPath, const StorePath & drvPathResolved, const OutputName & outputName)` (ctor) — delegates and adds trace pointing at the unresolved drv path.
- nlohmann adl_serializers: `DrvOutput::from_json/to_json` (object with `drvPath`/`outputName`); `UnkeyedRealisation::from_json/to_json` (object with `outPath`/`signatures`, `signatures` optional on read); `Realisation::from_json/to_json` (object with `key` and `value` where `value` is the unkeyed realisation).

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/realisation.hh

### Namespaces
- `nix`
- `std` (specialization of `std::hash` for `DrvOutput`).

### Classes / structs / enums
- Forward decls `class Store;`, `struct OutputsSpec;`.
- `struct DrvOutput`:
  - Fields `StorePath drvPath`, `OutputName outputName`.
  - `std::string to_string() const` (skips store dir).
  - Static `DrvOutput from_string(std::string_view)` (declaration only, no definition in this shard).
  - `std::string render(const StoreDirConfig & store) const` (with store dir).
  - Static `DrvOutput parse(const StoreDirConfig & store, std::string_view)`.
  - Default `bool operator==`/`auto operator<=>`.
- `struct UnkeyedRealisation`:
  - Fields `StorePath outPath`, `std::set<Signature> signatures`.
  - `std::string fingerprint(const DrvOutput & key) const`.
  - `Signature sign(const DrvOutput & key, const Signer &) const` (non-mutating).
  - `void sign(const DrvOutput & key, const Signer &)` (mutating).
  - `bool checkSignature(const DrvOutput & key, const PublicKeys &, const Signature &) const`.
  - `size_t checkSignatures(const DrvOutput & key, const PublicKeys &) const`.
  - Inline `bool isCompatibleWith(const UnkeyedRealisation & other) const` — compares only `outPath`.
  - Inline `const StorePath & getPath() const { return outPath; }`.
  - `GENERATE_CMP(UnkeyedRealisation, me->outPath)` — produces `==` and `<=>` on `outPath` only (signatures ignored).
- `struct Realisation : UnkeyedRealisation`:
  - Field `DrvOutput id`.
  - Default `bool operator==`/`auto operator<=>`.
- `typedef std::map<OutputName, UnkeyedRealisation> SingleDrvOutputs`.
- `struct OpaquePath`:
  - Field `StorePath path`.
  - Inline `const StorePath & getPath() const & { return path; }`.
  - Default `bool operator==`/`auto operator<=>`.
- `struct RealisedPath`:
  - `using Raw = std::variant<Realisation, OpaquePath>`.
  - Field `Raw raw`.
  - `using Set = std::set<RealisedPath>`.
  - Inline ctor `RealisedPath(StorePath path)` — wraps in `OpaquePath`.
  - Inline ctor `RealisedPath(Realisation r)`.
  - `const StorePath & path() const &`.
  - Default `bool operator==`/`auto operator<=>`.
- `class MissingRealisation final : public CloneableError<MissingRealisation, Error>`:
  - Inline ctor `MissingRealisation(const StoreDirConfig & store, DrvOutput & outputId)` — delegates.
  - Ctor `MissingRealisation(const StoreDirConfig & store, const StorePath & drvPath, const OutputName & outputName)`.
  - Ctor `MissingRealisation(const StoreDirConfig & store, const SingleDerivedPath & drvPath, const StorePath & drvPathResolved, const OutputName & outputName)`.
- `template<> struct std::hash<nix::DrvOutput>` — combines `drvPath` and `outputName` via `nix::hash_combine`.

### Functions
- `inline std::size_t hash_value(const DrvOutput & id)` (free) — `std::hash<DrvOutput>{}(id)`.

### Type aliases
- `typedef std::map<OutputName, UnkeyedRealisation> SingleDrvOutputs;`.

### Macros / globals
- `JSON_IMPL(nix::DrvOutput)`, `JSON_IMPL(nix::UnkeyedRealisation)`, `JSON_IMPL(nix::Realisation)`.

## File: src/libstore/make-content-addressed.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `std::map<StorePath, StorePath> makeContentAddressed(Store & srcStore, Store & dstStore, const StorePathSet & storePaths)` (free) — closure + topo-sort + reversed iteration; for each path, dumps NAR via `srcStore.narFromPath`, rewrites references using `remappings` (and self via `StoreReferences::self`), computes `narModuloHash` via `HashModuloSink`, builds a `FixedOutputInfo` (NixArchive method), then writes the rewritten NAR via a second `RewritingSink` (replacing old hashPart with new), records `narHash`/`narSize`, and calls `dstStore.addToStore`.
- `StorePath makeContentAddressed(Store & srcStore, Store & dstStore, const StorePath & fromPath)` (free) — single-path convenience that calls the set version and returns the mapping.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/include/nix/store/make-content-addressed.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `std::map<StorePath, StorePath> makeContentAddressed(Store & srcStore, Store & dstStore, const StorePathSet & rootPaths)`.
- `StorePath makeContentAddressed(Store & srcStore, Store & dstStore, const StorePath & rootPath)`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libstore/posix-fs-canonicalise.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined.

### Functions
- `static void canonicaliseTimestampAndPermissions(const std::filesystem::path & path, const PosixStat & st)` (file-local) — for non-symlinks, mask perms to `0444`/`0555` (preserving exec bit/dir flag); set mtime to `mtimeStore` (1) when not already (Unix only).
- `void canonicaliseTimestampAndPermissions(const std::filesystem::path & path)` (free) — convenience overload; `lstat`s first then dispatches.
- `static void canonicalisePathMetaData_(const std::filesystem::path & path, CanonicalizePathMetadataOptions options, InodesSeen & inodesSeen)` (file-local, recursive) — clears Apple `lchflags`; rejects non-regular/non-dir/non-symlink files; removes xattrs/ACLs (skipping `ignoredAcls`); validates ownership against `uidRange` (throws `BuildError(OutputRejected)` if outside, except for already-canonicalised hard links); `chmod`/`chown` to current user (Unix); recurses into directories using `DirectoryIterator`.
- `void canonicalisePathMetaData(const std::filesystem::path & path, CanonicalizePathMetadataOptions options, InodesSeen & inodesSeen)` (free) — public wrapper.
- `void canonicalisePathMetaData(const std::filesystem::path & path, CanonicalizePathMetadataOptions options)` (free) — overload creating fresh `InodesSeen`.

### Type aliases
None.

### Macros / globals
- `const time_t mtimeStore = 1;` — definition of canonical timestamp value (1 second into the epoch).

## File: src/libstore/include/nix/store/posix-fs-canonicalise.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `typedef std::pair<dev_t, ino_t> Inode;`.
- `typedef std::set<Inode> InodesSeen;`.
- `struct CanonicalizePathMetadataOptions`:
  - `std::optional<std::pair<uid_t, uid_t>> uidRange` (Unix only — guarded by `#ifndef _WIN32`).
  - `const StringSet & ignoredAcls` (when `NIX_SUPPORT_ACL`).
- `MakeError(PathInUse, Error)` — exception used elsewhere for in-use paths.

### Functions
- `void canonicalisePathMetaData(const std::filesystem::path &, CanonicalizePathMetadataOptions, InodesSeen &)`.
- `void canonicalisePathMetaData(const std::filesystem::path &, CanonicalizePathMetadataOptions)`.
- `void canonicaliseTimestampAndPermissions(const std::filesystem::path &)`.

### Type aliases
- `Inode`, `InodesSeen` (above).

### Macros / globals
- `#define NIX_WHEN_SUPPORT_ACLS(ARG) .ignoredAcls = ARG,` (when `NIX_SUPPORT_ACL`), else empty — designated-initializer helper for `CanonicalizePathMetadataOptions`.

## File: src/libstore/build-result.cc

### Namespaces
- `nix`
- `nlohmann` — adl_serializer specializations.

### Classes / structs / enums
None defined.

### Functions
- `void ExitStatusFlags::updateFromStatus(BuildResult::Failure::Status status)` (member) — sets flags based on the status enum (`TimedOut`/`HashMismatch`/`NotDeterministic`→`checkMismatch`/`PermanentFailure`+`InputRejected`→`permanentFailure`); switches under `-Wswitch-enum` suppression to allow subset.
- `unsigned int ExitStatusFlags::failingExitStatus() const` (member) — bitmask exit code: when there's a "problem with special exit code", sets bits per the documented mask (base `0b1100000`, then `+0b0100` for build failure, `+0b0001` for timeout, `+0b0010` for hash mismatch, `+0b1000` for check mismatch); falls back to `1` if no flags set.
- `bool BuildResult::operator==(const BuildResult &) const noexcept = default;`.
- `std::strong_ordering BuildResult::operator<=>(const BuildResult &) const noexcept = default;`.
- `bool BuildResult::Success::operator==(const BuildResult::Success &) const noexcept = default;`.
- `std::strong_ordering BuildResult::Success::operator<=>(const BuildResult::Success &) const noexcept = default;`.
- `static constexpr std::array<std::pair<BuildResult::Success::Status, std::string_view>, 4> successStatusStrings` (file-local) — status enum→string table built via `ENUM_ENTRY` macro.
- `static std::string_view successStatusToString(BuildResult::Success::Status status)` (file-local) — table lookup; throws on unknown.
- `static BuildResult::Success::Status successStatusFromString(std::string_view str)` (file-local) — reverse lookup; throws on unknown.
- `static constexpr std::array<std::pair<BuildResult::Failure::Status, std::string_view>, 12> failureStatusStrings` (file-local) — failure status table.
- `static std::string_view failureStatusToString(BuildResult::Failure::Status status)` (file-local).
- `static BuildResult::Failure::Status failureStatusFromString(std::string_view str)` (file-local).
- `bool BuildError::operator==(const BuildError & other) const noexcept` — compares `status`, `isNonDeterministic`, and `message()`.
- `std::strong_ordering BuildError::operator<=>(const BuildError & other) const noexcept` — lexicographic across the same fields.
- nlohmann adl_serializers: `BuildResult::to_json/from_json` (writes `success` discriminator, `status` string, common timing fields `timesBuilt`/`startTime`/`stopTime`/`cpuUser`/`cpuSystem`, plus success-specific `builtOutputs` or failure-specific `errorMsg`/`isNonDeterministic`); `KeyedBuildResult::to_json/from_json` (also embeds `path`).

### Type aliases
None.

### Macros / globals
- Local `#define ENUM_ENTRY(e) {BuildResult::Success::e, #e}` and the failure-side analog — used to build the status string tables, then `#undef`-ed.

## File: src/libstore/include/nix/store/build-result.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum struct BuildResultSuccessStatus : uint8_t { Built, Substituted, AlreadyValid, ResolvesToAlreadyValid }` (must be disjoint with `BuildResultFailureStatus` per comment).
- `enum struct BuildResultFailureStatus : uint8_t { PermanentFailure, InputRejected, OutputRejected, TransientFailure, CachedFailure (no longer used), TimedOut, MiscFailure, DependencyFailed, LogLimitExceeded, NotDeterministic, NoSubstituters, HashMismatch (a kind of OutputRejected, swapped before serialization) }`.
- `struct BuildError : public CloneableError<BuildError, Error>`:
  - `using Status = BuildResultFailureStatus`; `using enum Status` (imports unqualified enumerators).
  - Field `Status status = MiscFailure`.
  - Field `bool isNonDeterministic = false`.
  - Variadic ctor `template<typename... Args> BuildError(Status status, const Args &... args)`.
  - Nested `struct Args { Status status; HintFmt msg; bool isNonDeterministic = false; }`.
  - Ctor `BuildError(Args args)` — also used for deserialization.
  - Default ctor `BuildError()` — empty message.
  - `bool operator==(const BuildError &) const noexcept`.
  - `std::strong_ordering operator<=>(const BuildError &) const noexcept`.
- `struct BuildResult`:
  - Nested `struct Success`:
    - `using Status = enum BuildResultSuccessStatus`; `using enum Status`.
    - Field `Status status`.
    - Field `SingleDrvOutputs builtOutputs`.
    - `bool operator==(const BuildResult::Success &) const noexcept`; `std::strong_ordering operator<=>(const BuildResult::Success &) const noexcept`.
  - `using Failure = BuildError`.
  - Field `std::variant<Success, Failure> inner = Failure{}`.
  - Member template `auto * tryGetSuccess(this auto & self)` — deducing-`this` overload returning `std::get_if<Success>(&self.inner)`.
  - Member template `auto * tryGetFailure(this auto & self)` — deducing-`this` overload.
  - `void tryThrowBuildError(std::optional<unsigned int> exitStatus = std::nullopt)` — if failure variant, calls `withExitStatus` and throws.
  - Field `unsigned int timesBuilt = 0`.
  - Fields `time_t startTime = 0, stopTime = 0`.
  - Fields `std::optional<std::chrono::microseconds> cpuUser, cpuSystem`.
  - `bool operator==(const BuildResult &) const noexcept`; `std::strong_ordering operator<=>(const BuildResult &) const noexcept`.
- `struct KeyedBuildResult : BuildResult`:
  - Field `DerivedPath path`.
  - Ctor `KeyedBuildResult(BuildResult res, DerivedPath path)` (workaround for GCC uninitialized-warning).
- `struct ExitStatusFlags`:
  - Bool fields: `permanentFailure = false`, `timedOut = false`, `hashMismatch = false`, `checkMismatch = false`.
  - `void updateFromStatus(BuildResult::Failure::Status status)`.
  - `unsigned int failingExitStatus() const`.

### Functions
None outside structs.

### Type aliases
None outside structs.

### Macros / globals
- `JSON_IMPL(nix::BuildResult)`, `JSON_IMPL(nix::KeyedBuildResult)`.

## Cross-file observations

- **Variant-with-named-cases idiom**: `ContentAddressMethod` (`Raw` enum: `Flat`/`NixArchive`/`Git`/`Text`), `ContentAddressWithReferences` (variant `TextInfo`/`FixedOutputInfo`), `OutputsSpec` (variant `All`/`Names`), `ExtendedOutputsSpec` (variant `Default`/`Explicit`), `SingleDerivedPath`/`DerivedPath` (variant `Opaque`/`Built`), `RealisedPath` (variant `Realisation`/`OpaquePath`), `StorePathWithOutputs::ParseResult` (`StorePathWithOutputs`/`StorePath`/`std::monostate`), `StoreReference::Variant` (`Auto`/`Specified`/`Daemon`/`Local`), `BuildResult::inner` (`Success`/`Failure`), `DrvRef<Item>` (`OutputName`/`Item`). All use `std::visit` + `overloaded` plus either `MAKE_WRAPPER_CONSTRUCTOR` or a public `Raw raw` member. Each pair commonly redefines `==`, `to_string`, and `parse` symmetrically; there may be room for a tagged-union helper.
- **JSON adl_serializer boilerplate** appears in nearly every file (`store-reference.cc`, `path.cc`, `path-info.cc`, `derived-path.cc`, `downstream-placeholder.cc`, `outputs-spec.cc`, `realisation.cc`, `content-address.cc`, `build-result.cc`). Each uses the same pattern of `getObject`/`valueAt`/`optionalValueAt`/`getString`/`getBoolean`/`getUnsigned` from `json-utils.hh`. The `JSON_IMPL`, `JSON_IMPL_WITH_XP_FEATURES`, and `json_avoids_null<T>` declarations are duplicated.
- **`GENERATE_CMP` / `GENERATE_CMP_EXT` / `GENERATE_EQUAL` / `GENERATE_ONE_CMP` macros** appear in `path-info.cc`, `derived-path.cc`, `realisation.hh`. Multiple TODO comments throughout (`content-address.hh`, `derived-path.hh`, `derived-path-map.hh`, `derived-path-map.cc`, `outputs-spec.hh`) cite the same libc++16 (Apple) limitation: missing `std::map::operator<=>` / `std::set::operator<=>`. Once the toolchain ratchet moves up, these can collapse to `auto operator<=>(...) = default;`.
- **`StoreDirConfig::print*` / `parse*` symmetry**: `parseStorePath`, `printStorePath`, `parseStorePathSet`, `printStorePathSet`, plus `to_string`/`render` methods on `StorePath`, `DrvOutput`, `StorePathWithOutputs`, `DerivedPath`, `SingleDerivedPath` all duplicate the `<storeDir>/<base>` formatting. Renderers that omit the store dir live on `StorePath::to_string` and `DrvOutput::to_string`; renderers that include it live on `printStorePath` and `DrvOutput::render`. There is no shared "formattable store object" abstraction.
- **Recursive resolution with shared cache**: `outputs-query.cc` carefully threads `ResolveCache` and `RealisationCache` through recursive helpers (`deepQueryPartialDerivationOutputImpl`, `resolveSingleDerivedPath`, `resolveDerivation`); `derived-path-map.cc` has a similar pattern of recursive `std::visit` over `SingleDerivedPath` (`ensureSlot`, `findSlot`, `removeSlot`). Both could share an "iterate over a derived path tree" helper, although they have different traversal contracts (`derived-path-map` modifies state, `outputs-query` accumulates).
- **Reference-scanning sinks**: `references.cc` and `path-references.cc` both produce `StorePathSet` outputs from byte streams via `RefScanSink` derivatives (`PathRefScanSink` adds a `backMap`); `make-content-addressed.cc` then uses `RewritingSink`/`HashModuloSink` from `references.hh` to rewrite a NAR. Each user implementing the hash→`StorePath` lookup re-creates the same `hashPart -> StorePath` mapping pattern.
- **Async/sync pair pattern**: `Store::queryPathInfo` and `Store::queryRealisation` each have a synchronous `promise/future` wrapper around a `Callback`-based async variant. The wrapper code is structurally identical and could be factored into a helper template that turns any async callback into a blocking call.
- **Disk-cache lookup branching**: `Store::isValidPath`, `Store::queryPathInfo` (async), `Store::queryPathInfoFromClientCache`, and `Store::queryRealisation` (async) all hit the same disk-cache → in-memory cache → upstream chain with subtly different parameters. `isValidPath` and `queryPathInfoFromClientCache` are particularly close in shape (the latter is essentially "what would `isValidPath` return without the upstream call, plus the info itself").
- **`unsupported(...)`** is used as a default for many virtuals (`queryAllValidPaths`, `queryReferrers`, `addSignatures`). This is a workaround in lieu of pure-virtual + capability traits; explicit feature negotiation might be cleaner.
- **`StoreConfig`/`Store` separation**: `StoreConfig` and `Store` both inherit `StoreDirConfig`; the header itself flags this with a `@todo` (the comment on `StoreDirConfig` says it should just be inherited by `StoreConfig`). The `anchor()` vtable workaround appears on both `StoreConfig` and `Store` to avoid Darwin shared-library `dynamic_cast` problems. There is duplication of "store dir setting" between `StoreConfigBase::storeDir_` (the actual `Setting<std::string>`) and `StoreDirConfig::storeDir` (a `const std::string &` reference into it) — `StoreConfig`'s constructor wires them up explicitly.
- **`PathFmt(...)` formatter** appears in numerous error messages (canonicalisation, store-api, store-dir-config, path-references) — used for cross-platform path printing.
- **`copyPaths` overloads vs `copyClosure` overloads** in `store-api.cc` parallel each other for `StorePathSet` and `RealisedPath::Set`. Each pair could be reduced via a small adapter (the `RealisedPath::Set` variant just iterates and inserts opaques into the closure, then registers realisations after path copy).
- **`signPathInfo` / `signRealisation`** in `store-api.cc` share identical structure: load each `secretKeyFiles` entry, construct a `LocalSigner`, call the appropriate `sign` method. Could be extracted into a shared "for each configured signer" helper.
- **Duplicated config plumbing in `auto`/store-registration**: `resolveStoreConfig`'s `Auto` branch instantiates a local `TempLocalFSStoreConfig` solely to access `LocalFSStoreConfig.stateDir.get()` for the existence check before deciding between `LocalStore::Config` / `UDSRemoteStore::Config` / chroot store. This pattern of "construct a throwaway config to read derived defaults" arises because settings derive their defaults at config-time rather than via static helpers.
- **`UnkeyedRealisation`/`Realisation`/`ValidPathInfo`/`UnkeyedValidPathInfo` parallel structure**: realisations have an "unkeyed" form (just `outPath` + `signatures`) and a "keyed" form (adds `id`); path info similarly has `UnkeyedValidPathInfo` (no path) and `ValidPathInfo` (adds `path`). The fingerprint-then-sign-then-verify protocol appears in both (`ValidPathInfo::fingerprint`/`sign`/`checkSignature` and `UnkeyedRealisation::fingerprint`/`sign`/`checkSignature`).
- **Hash-modulo-self pattern**: `make-content-addressed.cc` uses `HashModuloSink` (with the old `hashPart` as modulus) to compute a hash that's invariant under self-references, then writes a second `RewritingSink` to actually substitute the new hashPart. Self-reference handling appears in multiple places: `StoreReferences::self`, `ValidPathInfo::contentAddressWithReferences` (splitting self vs others), and `ValidPathInfo::makeFromCA` (re-inserting self into `references` for FixedOutputInfo).

