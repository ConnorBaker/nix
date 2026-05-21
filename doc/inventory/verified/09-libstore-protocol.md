# Inventory — Shard 09: libstore protocols

## File: src/libstore/worker-protocol.cc

### Namespaces
- `nix` — namespace, src/libstore/worker-protocol.cc — encloses all definitions in this translation unit.

### Classes / structs / enums
(none defined here; the file only adds method bodies for `WorkerProto::Serialise<T>` specializations declared in the header)

### Functions
- `WorkerProto::latest` — `const Version` definition, src/libstore/worker-protocol.cc — current worker-protocol version 1.38 plus declared features (`featureRealisationWithPath`, `featureDeleteDeadSpecificReferrers`).
- `WorkerProto::minimum` — `const Version` definition, src/libstore/worker-protocol.cc — minimum supported wire version 1.18 (no features).
- `WorkerProto::Version::operator<=>` — out-of-class member definition, src/libstore/worker-protocol.cc — partial ordering; uses `std::includes` on feature sets and combines with the `number <=> other.number` result; returns `unordered` when neither side's features subset-includes the other.
- `WorkerProto::Serialise<BuildMode>::read` — static member, src/libstore/worker-protocol.cc — decodes BuildMode from one-byte tag (0/1/2 → bmNormal/bmRepair/bmCheck); throws on other values.
- `WorkerProto::Serialise<BuildMode>::write` — static member, src/libstore/worker-protocol.cc — encodes BuildMode as one-byte tag.
- `WorkerProto::Serialise<GCAction>::read` — static member, src/libstore/worker-protocol.cc — decodes `unsigned` tag 0..3 → gcReturnLive/gcReturnDead/gcDeleteDead/gcDeleteSpecific; throws on other values.
- `WorkerProto::Serialise<GCAction>::write` — static member, src/libstore/worker-protocol.cc — encodes GCAction as `unsigned`.
- `WorkerProto::Serialise<std::optional<TrustedFlag>>::read` — static member, src/libstore/worker-protocol.cc — three-state byte tag: 0=nullopt, 1=Trusted, 2=NotTrusted.
- `WorkerProto::Serialise<std::optional<TrustedFlag>>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer for the three-state tag.
- `WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::read` — static member, src/libstore/worker-protocol.cc — one-byte present-bit (0/1); when present reads `int64_t` count and constructs `std::chrono::microseconds`.
- `WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer.
- `WorkerProto::Serialise<DerivedPath>::read` — static member, src/libstore/worker-protocol.cc — version-gated parsing: ≥1.30 uses `DerivedPath::parseLegacy`, else `parsePathWithOutputs(...).toDerivedPath()`.
- `WorkerProto::Serialise<DerivedPath>::write` — static member, src/libstore/worker-protocol.cc — ≥1.30 emits `req.to_string_legacy(store)`; below 1.30 uses `StorePathWithOutputs::tryFromDerivedPath` and throws if the request is a bare drv path or a build-of-build-product.
- `WorkerProto::Serialise<KeyedBuildResult>::read` — static member, src/libstore/worker-protocol.cc — composes DerivedPath + BuildResult.
- `WorkerProto::Serialise<KeyedBuildResult>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; downcasts the KeyedBuildResult to `BuildResult` for the body.
- `WorkerProto::Serialise<BuildResult>::read` — static member, src/libstore/worker-protocol.cc — reads BuildResultStatus + errorMsg + (≥1.29) timesBuilt/isNonDeterministic/startTime/stopTime + (≥1.37) cpuUser/cpuSystem + builtOutputs (new map keyed on outputName when feature `realisation-with-path-not-hash` present, else legacy StringMap with `sha256:hex!outputName` keys when ≥1.28); fuses `Success`/`Failure` into the `BuildResult::inner` variant via `std::visit`.
- `WorkerProto::Serialise<BuildResult>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; uses a `common` lambda to share the success/failure payload layout, with a dummy hash for the legacy StringMap branch.
- `WorkerProto::Serialise<ValidPathInfo>::read` — static member, src/libstore/worker-protocol.cc — composes StorePath + UnkeyedValidPathInfo.
- `WorkerProto::Serialise<ValidPathInfo>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer (downcast to `UnkeyedValidPathInfo`).
- `WorkerProto::Serialise<UnkeyedValidPathInfo>::read` — static member, src/libstore/worker-protocol.cc — reads optional deriver / narHash (Base16, no prefix) / references / registrationTime / narSize, and (≥1.16) ultimate / sigs / ca.
- `WorkerProto::Serialise<UnkeyedValidPathInfo>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer.
- `WorkerProto::Serialise<WorkerProto::ClientHandshakeInfo>::read` — static member, src/libstore/worker-protocol.cc — daemonNixVersion (≥1.33) + optional TrustedFlag (≥1.35; below 1.35 leaves `remoteTrustsUs = nullopt`).
- `WorkerProto::Serialise<WorkerProto::ClientHandshakeInfo>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; asserts `info.daemonNixVersion` when version ≥1.33.
- `WorkerProto::Serialise<UnkeyedRealisation>::read` — static member, src/libstore/worker-protocol.cc — outPath + signatures; throws if `featureRealisationWithPath` not negotiated.
- `WorkerProto::Serialise<UnkeyedRealisation>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; same feature gate.
- `WorkerProto::Serialise<std::optional<UnkeyedRealisation>>::read` — static member, src/libstore/worker-protocol.cc — when feature absent, swallows a discarded string for back-compat and returns `nullopt`; otherwise reads a 0/1 present-tag plus body.
- `WorkerProto::Serialise<std::optional<UnkeyedRealisation>>::write` — static member, src/libstore/worker-protocol.cc — present-bit + body; no special handling when feature absent on write.
- `WorkerProto::Serialise<DrvOutput>::read` — static member, src/libstore/worker-protocol.cc — drvPath + outputName; throws if `featureRealisationWithPath` not negotiated.
- `WorkerProto::Serialise<DrvOutput>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; same feature gate.
- `WorkerProto::Serialise<Realisation>::read` — static member, src/libstore/worker-protocol.cc — DrvOutput id + UnkeyedRealisation body.
- `WorkerProto::Serialise<Realisation>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer; downcasts to `UnkeyedRealisation` for body.
- `WorkerProto::Serialise<GCOptions::SpecificPaths>::read` — static member, src/libstore/worker-protocol.cc — paths + `deleteReferrers` bool.
- `WorkerProto::Serialise<GCOptions::SpecificPaths>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer.
- `WorkerProto::Serialise<GCOptions::GCPaths>::read` — static member, src/libstore/worker-protocol.cc — one-byte tag: 0 → SpecificPaths body, 1 → WholeStore (no body); throws on other values.
- `WorkerProto::Serialise<GCOptions::GCPaths>::write` — static member, src/libstore/worker-protocol.cc — symmetric writer using `std::visit` over the variant.

### Type aliases
(none)

### Macros / globals
(none defined here; the constants `WorkerProto::latest` and `WorkerProto::minimum` are listed under Functions/globals above)

---

## File: src/libstore/include/nix/store/worker-protocol.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/worker-protocol.hh — encloses worker-protocol public API.

### Classes / structs / enums
- `WorkerProto` — struct (acts as namespace; usable as template argument), src/libstore/include/nix/store/worker-protocol.hh — top-level type holding everything related to the worker protocol.
- `WorkerProto::Op` — `enum struct : uint64_t` (forward in `WorkerProto`, definition out-of-line below), src/libstore/include/nix/store/worker-protocol.hh — wire opcodes for the worker protocol; live cases: IsValidPath=1, QueryReferrers=6, AddToStore=7, AddTextToStore=8 (obsolete since 1.25), BuildPaths=9, EnsurePath=10, AddTempRoot=11, AddIndirectRoot=12, SyncWithGC=13, FindRoots=14, QueryDeriver=18 (obsolete), SetOptions=19, CollectGarbage=20, QuerySubstitutablePathInfo=21, QueryDerivationOutputs=22 (obsolete), QueryAllValidPaths=23, QueryPathInfo=26, QueryDerivationOutputNames=28 (obsolete), QueryPathFromHashPart=29, QuerySubstitutablePathInfos=30, QueryValidPaths=31, QuerySubstitutablePaths=32, QueryValidDerivers=33, OptimiseStore=34, VerifyStore=35, BuildDerivation=36, AddSignatures=37, NarFromPath=38, AddToStoreNar=39, QueryMissing=40, QueryDerivationOutputMap=41, RegisterDrvOutput=42, QueryRealisation=43, AddMultipleToStore=44, AddBuildLog=45, BuildPathsWithResults=46, AddPermRoot=47.
- `WorkerProto::Version` — struct, src/libstore/include/nix/store/worker-protocol.hh — bundles a `Number` and a `FeatureSet`; partial ordering only on the full type.
- `WorkerProto::Version::Number` — nested struct, src/libstore/include/nix/store/worker-protocol.hh — `unsigned int major; uint8_t minor;` with `operator<=>=default` (total) plus `toWire`/`fromWire`.
- `WorkerProto::ReadConn` — struct, src/libstore/include/nix/store/worker-protocol.hh — pair `Source & from; const Version & version;` for read direction.
- `WorkerProto::WriteConn` — struct, src/libstore/include/nix/store/worker-protocol.hh — pair `Sink & to; const Version & version;` for write direction.
- `WorkerProto::BasicConnection` — struct (forward declaration here, defined in worker-protocol-connection.hh), src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::BasicClientConnection` — struct (forward declaration here), src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::BasicServerConnection` — struct (forward declaration here), src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::ClientHandshakeInfo` — struct, src/libstore/include/nix/store/worker-protocol.hh — `optional<string> daemonNixVersion`, `optional<TrustedFlag> remoteTrustsUs`, `operator==` defaulted.
- `WorkerProto::Serialise<T>` — primary template (declared but intentionally undefined globally), src/libstore/include/nix/store/worker-protocol.hh — must be specialized per type; generic catch-all defined in worker-protocol-impl.hh.

### Functions
- `WorkerProto::latest` — `static const Version` declaration, src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::minimum` — `static const Version` declaration, src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::Version::Number::operator<=>` — defaulted (total ordering), src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::Version::Number::toWire` — `constexpr` member, src/libstore/include/nix/store/worker-protocol.hh — `(major << 8) | minor`.
- `WorkerProto::Version::Number::fromWire` — `constexpr` static member, src/libstore/include/nix/store/worker-protocol.hh — inverse.
- `WorkerProto::Version::operator==` — defaulted, src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::Version::operator<=>` — declaration (returns `std::partial_ordering`; defined in `.cc`), src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::write<T>` — static template wrapper, src/libstore/include/nix/store/worker-protocol.hh — forwards to `Serialise<T>::write` for type inference.
- `WorkerProto::ClientHandshakeInfo::operator==` — defaulted, src/libstore/include/nix/store/worker-protocol.hh.
- `operator<<(Sink &, WorkerProto::Op)` — free function (inline), src/libstore/include/nix/store/worker-protocol.hh — sends op code as `uint64_t`.
- `operator<<(std::ostream &, WorkerProto::Op)` — free function (inline), src/libstore/include/nix/store/worker-protocol.hh — debug printing for op code as `uint64_t`.

### Type aliases
- `WorkerProto::Version::Feature` — `using Feature = std::string`, src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::Version::FeatureSet` — `using FeatureSet = std::set<Feature, std::less<>>`, src/libstore/include/nix/store/worker-protocol.hh.

### Macros / globals
- `WORKER_MAGIC_1` (0x6e697863), `WORKER_MAGIC_2` (0x6478696f) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — handshake magic constants.
- `PROTOCOL_VERSION` ((1<<8)|39) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — declared current wire version constant. Note: `WorkerProto::latest.number` is actually 1.38; the macro is not consumed by this header itself.
- `MINIMUM_PROTOCOL_VERSION` ((1<<8)|18) — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `GET_PROTOCOL_MAJOR(x)` `((x) & 0xff00)` — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `GET_PROTOCOL_MINOR(x)` `((x) & 0x00ff)` — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `STDERR_NEXT` (0x6f6c6d67) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — log-line message tag.
- `STDERR_READ` (0x64617461) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — server-requests-data tag.
- `STDERR_WRITE` (0x64617416) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — server-pushes-data tag.
- `STDERR_LAST` (0x616c7473) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — terminator tag.
- `STDERR_ERROR` (0x63787470) — `#define`, src/libstore/include/nix/store/worker-protocol.hh — error tag.
- `STDERR_START_ACTIVITY` (0x53545254) — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `STDERR_STOP_ACTIVITY` (0x53544f50) — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `STDERR_RESULT` (0x52534c54) — `#define`, src/libstore/include/nix/store/worker-protocol.hh.
- `WorkerProto::featureRealisationWithPath` — `static constexpr std::string_view`, src/libstore/include/nix/store/worker-protocol.hh — opt-in feature flag literal `"realisation-with-path-not-hash"`.
- `WorkerProto::featureDeleteDeadSpecificReferrers` — `static constexpr std::string_view`, src/libstore/include/nix/store/worker-protocol.hh — `"delete-dead-specific-referrers"`.
- `WorkerProto::featureDisableSetOptions` — `static constexpr std::string_view`, src/libstore/include/nix/store/worker-protocol.hh — `"disable-set-options"` (used in recursive-nix daemon mode).
- `DECLARE_WORKER_SERIALISER(T)` — `#define`, src/libstore/include/nix/store/worker-protocol.hh — emits `struct WorkerProto::Serialise<T> { static T read(...); static void write(...); };`.
- `COMMA_` — `#define ,` then `#undef`, src/libstore/include/nix/store/worker-protocol.hh — local helper to allow templated specializations to contain commas.
- Worker-proto serialise specializations declared via `template<> DECLARE_WORKER_SERIALISER(...)` — DerivedPath, BuildResult, KeyedBuildResult, ValidPathInfo, UnkeyedValidPathInfo, DrvOutput, UnkeyedRealisation, Realisation, std::optional<UnkeyedRealisation>, BuildMode, GCAction, std::optional<TrustedFlag>, std::optional<std::chrono::microseconds>, WorkerProto::ClientHandshakeInfo, GCOptions::SpecificPaths, GCOptions::GCPaths, std::vector<T>, std::set<T,Compare>, std::tuple<Ts...>, std::map<K,V,Compare>.

---

## File: src/libstore/include/nix/store/worker-protocol-impl.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/worker-protocol-impl.hh.

### Classes / structs / enums
- `WorkerProto::Serialise<T>` — primary template definition (only enabled now via this impl header), src/libstore/include/nix/store/worker-protocol-impl.hh — generic fallback that delegates to `CommonProto::Serialise<T>` by constructing a `CommonProto::ReadConn`/`WriteConn` from the worker conn's `from`/`to`. This is the bridge between the worker and common protocol families.

### Functions
- `WorkerProto::Serialise<std::vector<T>>::{read,write}` — defined via `WORKER_USE_LENGTH_PREFIX_SERIALISER`, src/libstore/include/nix/store/worker-protocol-impl.hh — length-prefixed container serializer.
- `WorkerProto::Serialise<std::set<T,Compare>>::{read,write}` — same mechanism, src/libstore/include/nix/store/worker-protocol-impl.hh.
- `WorkerProto::Serialise<std::tuple<Ts...>>::{read,write}` — same mechanism, src/libstore/include/nix/store/worker-protocol-impl.hh.
- `WorkerProto::Serialise<std::map<K,V,Compare>>::{read,write}` — same mechanism, src/libstore/include/nix/store/worker-protocol-impl.hh.

### Type aliases
(none)

### Macros / globals
- `WORKER_USE_LENGTH_PREFIX_SERIALISER(TEMPLATE, T)` — `#define`, src/libstore/include/nix/store/worker-protocol-impl.hh — emits `read`/`write` static members that delegate to `LengthPrefixedProtoHelper<WorkerProto, T>`.
- `COMMA_` — `#define ,` / `#undef`, src/libstore/include/nix/store/worker-protocol-impl.hh — local helper for the `std::set` specialization.
- `WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA` — `#define ,`, src/libstore/include/nix/store/worker-protocol-impl.hh — used in the `std::map` specialization (no matching `#undef`).

---

## File: src/libstore/worker-protocol-connection.cc

### Namespaces
- `nix` — namespace, src/libstore/worker-protocol-connection.cc.

### Classes / structs / enums
(none defined here; only method bodies for `WorkerProto::Basic*Connection`)

### Functions
- `WorkerProto::BasicClientConnection::~BasicClientConnection` — destructor, src/libstore/worker-protocol-connection.cc — flushes `to`, swallows exceptions via `ignoreExceptionInDestructor`.
- `readFields` — file-static free function, src/libstore/worker-protocol-connection.cc — reads a `Logger::Fields` vector (size-prefixed sequence of int/string fields, type tag per element).
- `WorkerProto::BasicClientConnection::processStderrReturn` — member, src/libstore/worker-protocol-connection.cc — drives the client-side STDERR_* dispatch loop: WRITE→sink, READ→source (writes back framed reply), ERROR→exception_ptr (full Error ≥1.26, else `(status, msg)` tuple), NEXT→`printError`, START/STOP_ACTIVITY/RESULT→logger callbacks; LAST/EOF terminates. Includes a back-compat hack for pre-#4628 daemons reporting dynamic-derivation parse errors when `Xp::DynamicDerivations` is enabled and proto < 1.36.
- `WorkerProto::BasicClientConnection::processStderr` — member, src/libstore/worker-protocol-connection.cc — wrapper that sets `*daemonException = true` and rethrows the exception_ptr returned by `processStderrReturn`.
- `intersectFeatures` — file-static free function, src/libstore/worker-protocol-connection.cc — set intersection of two `Version::FeatureSet`s used during handshake negotiation.
- `WorkerProto::BasicClientConnection::handshake` — static, src/libstore/worker-protocol-connection.cc — sends WORKER_MAGIC_1+localVersion.number, validates daemon's WORKER_MAGIC_2, requires same major and ≥1.10; takes the min of the two version numbers; when the agreed minor ≥1.38 exchanges feature sets and intersects them.
- `WorkerProto::BasicServerConnection::handshake` — static, src/libstore/worker-protocol-connection.cc — server-side mirror.
- `WorkerProto::BasicClientConnection::postHandshake` — member, src/libstore/worker-protocol-connection.cc — sends obsolete CPU-affinity int (≥1.14) and obsolete `reserveSpace` flag (≥1.11); flushes ≥1.33; reads `ClientHandshakeInfo`.
- `WorkerProto::BasicServerConnection::postHandshake` — member, src/libstore/worker-protocol-connection.cc — server mirror; reads obsolete bytes, writes `ClientHandshakeInfo`.
- `WorkerProto::BasicClientConnection::queryPathInfo` — member, src/libstore/worker-protocol-connection.cc — sends `Op::QueryPathInfo` + path, drives stderr; handles "is not valid" backward-compat error as `nullopt`; ≥1.17 reads a one-byte presence flag before the body.
- `WorkerProto::BasicClientConnection::queryValidPaths` — member, src/libstore/worker-protocol-connection.cc — asserts ≥1.12, sends `Op::QueryValidPaths` + paths, plus optional `maybeSubstitute` flag (≥1.27); reads back the resulting StorePathSet.
- `WorkerProto::BasicClientConnection::addTempRoot` — member, src/libstore/worker-protocol-connection.cc — sends `Op::AddTempRoot` + path, drains response int.
- `WorkerProto::BasicClientConnection::putBuildDerivationRequest` — member, src/libstore/worker-protocol-connection.cc — sends `Op::BuildDerivation` + drv path + serialized derivation (`writeDerivation`) + buildMode.
- `WorkerProto::BasicClientConnection::getBuildDerivationResponse` — member, src/libstore/worker-protocol-connection.cc — reads BuildResult.
- `WorkerProto::BasicClientConnection::narFromPath` — member, src/libstore/worker-protocol-connection.cc — sends `Op::NarFromPath` + path, drives stderr, hands the resulting Source to the `receiveNar` callback.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libstore/include/nix/store/worker-protocol-connection.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/worker-protocol-connection.hh.

### Classes / structs / enums
- `WorkerProto::BasicConnection` — struct, src/libstore/include/nix/store/worker-protocol-connection.hh — holds bidirectional `FdSink to`, `FdSource from`, negotiated `WorkerProto::Version protoVersion`. Provides implicit `operator ReadConn()` and `operator WriteConn()` so the connection can be used directly with the unidirectional connection types expected by serializers.
- `WorkerProto::BasicClientConnection` — struct deriving `BasicConnection`, src/libstore/include/nix/store/worker-protocol-connection.hh — virtual destructor + pure virtual `closeWrite()`; declares all client-side helpers used to talk to the daemon.
- `WorkerProto::BasicServerConnection` — struct deriving `BasicConnection`, src/libstore/include/nix/store/worker-protocol-connection.hh — server-side handshake/postHandshake helpers.

### Functions
- `WorkerProto::BasicConnection::operator WorkerProto::ReadConn()` — implicit conversion, src/libstore/include/nix/store/worker-protocol-connection.hh — produces `{from, protoVersion}`.
- `WorkerProto::BasicConnection::operator WorkerProto::WriteConn()` — implicit conversion, src/libstore/include/nix/store/worker-protocol-connection.hh — produces `{to, protoVersion}`.
- `WorkerProto::BasicClientConnection::~BasicClientConnection` — virtual destructor (defined in cc), src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::closeWrite` — pure virtual, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::processStderrReturn` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::processStderr` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::handshake` — static declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::postHandshake` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::addTempRoot` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::queryValidPaths` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::queryPathInfo` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::putBuildDerivationRequest` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::getBuildDerivationResponse` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicClientConnection::narFromPath` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicServerConnection::handshake` — static declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.
- `WorkerProto::BasicServerConnection::postHandshake` — member declaration, src/libstore/include/nix/store/worker-protocol-connection.hh.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libstore/include/nix/store/worker-settings.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/worker-settings.hh.

### Classes / structs / enums
- `MaxBuildJobsSetting` — struct deriving `BaseSetting<unsigned int>`, src/libstore/include/nix/store/worker-settings.hh — overrides `parse` to allow special value `"auto"`.
- `WorkerSettings` — struct deriving `virtual Config`, src/libstore/include/nix/store/worker-settings.hh — bundle of build-side settings consumed by daemon and workers.

### Functions
- `MaxBuildJobsSetting::MaxBuildJobsSetting` — constructor (inline), src/libstore/include/nix/store/worker-settings.hh — calls `BaseSetting<unsigned int>` and registers itself on the owning `Config` via `options->addSetting(this)`.
- `MaxBuildJobsSetting::parse` — `unsigned int parse(const std::string &) const override` declaration, src/libstore/include/nix/store/worker-settings.hh.
- `WorkerSettings::WorkerSettings` — protected `= default` constructor, src/libstore/include/nix/store/worker-settings.hh.

### Type aliases
(none)

### Macros / globals
- `WorkerSettings::keepGoing` — `Setting<bool>` (default false), src/libstore/include/nix/store/worker-settings.hh — `keep-going`.
- `WorkerSettings::tryFallback` — `Setting<bool>` (default false), src/libstore/include/nix/store/worker-settings.hh — `fallback` (alias `build-fallback`).
- `WorkerSettings::logLines` — `Setting<size_t>` (default 25), src/libstore/include/nix/store/worker-settings.hh — `log-lines`.
- `WorkerSettings::maxBuildJobs` — `MaxBuildJobsSetting` (default 1), src/libstore/include/nix/store/worker-settings.hh — `max-jobs` (alias `build-max-jobs`).
- `WorkerSettings::maxSubstitutionJobs` — `Setting<unsigned int>` (default 16), src/libstore/include/nix/store/worker-settings.hh — `max-substitution-jobs` (alias `substitution-max-jobs`).
- `WorkerSettings::maxSilentTime` — `Setting<time_t>` (default 0), src/libstore/include/nix/store/worker-settings.hh — `max-silent-time` (alias `build-max-silent-time`).
- `WorkerSettings::buildTimeout` — `Setting<time_t>` (default 0), src/libstore/include/nix/store/worker-settings.hh — `timeout` (alias `build-timeout`).
- `WorkerSettings::buildHook` — `Setting<Strings>` (default `{"nix", "__build-remote"}`), src/libstore/include/nix/store/worker-settings.hh — `build-hook`.
- `WorkerSettings::builders` — `Setting<std::string>` (default `"@" + (nixConfDir() / "machines").string()`, with `documentDefault=false`), src/libstore/include/nix/store/worker-settings.hh — `builders`.
- `WorkerSettings::alwaysAllowSubstitutes` — `Setting<bool>` (default false), src/libstore/include/nix/store/worker-settings.hh — `always-allow-substitutes`.
- `WorkerSettings::buildersUseSubstitutes` — `Setting<bool>` (default false), src/libstore/include/nix/store/worker-settings.hh — `builders-use-substitutes`.
- `WorkerSettings::useSubstitutes` — `Setting<bool>` (default true), src/libstore/include/nix/store/worker-settings.hh — `substitute` (alias `build-use-substitutes`).
- `WorkerSettings::substituters` — `Setting<std::vector<StoreReference>>` (default `https://cache.nixos.org/`), src/libstore/include/nix/store/worker-settings.hh — `substituters` (alias `binary-caches`).
- `WorkerSettings::maxLogSize` — `Setting<unsigned long>` (default 0), src/libstore/include/nix/store/worker-settings.hh — `max-build-log-size` (alias `build-max-log-size`).
- `WorkerSettings::pollInterval` — `Setting<unsigned int>` (default 5), src/libstore/include/nix/store/worker-settings.hh — `build-poll-interval`.
- `WorkerSettings::postBuildHook` — `Setting<std::string>` (default ""), src/libstore/include/nix/store/worker-settings.hh — `post-build-hook`.

---

## File: src/libstore/serve-protocol.cc

### Namespaces
- `nix` — namespace, src/libstore/serve-protocol.cc.

### Classes / structs / enums
(none defined here; method bodies only)

### Functions
- `ServeProto::Serialise<BuildResult>::read` — static member, src/libstore/serve-protocol.cc — reads BuildResultStatus + errorMsg + (≥2.3) timesBuilt/isNonDeterministic/startTime/stopTime + builtOutputs (new map ≥2.8 / legacy StringMap with `sha256:hex!outputName` keys ≥2.6); fuses success/failure into `BuildResult::inner` via `std::visit`.
- `ServeProto::Serialise<BuildResult>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer using a shared `common` lambda; the serve-side variant has no cpu-timing or feature-flag branch.
- `ServeProto::Serialise<UnkeyedValidPathInfo>::read` — static member, src/libstore/serve-protocol.cc — initialises with `Hash::dummy`; reads deriver (empty-string sentinel = absent) / refs / discarded download size / narSize, plus (≥2.4) narHash (`Hash::parseAnyPrefixed`, may be empty) / ca / sigs.
- `ServeProto::Serialise<UnkeyedValidPathInfo>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer; emits empty string when `info.deriver` is absent, sends `narSize` twice (the first as the obsolete "downloadSize" "lying a little"), and (≥2.4) emits narHash in `HashFormat::Nix32` (with prefix), ca, sigs.
- `ServeProto::Serialise<ServeProto::BuildOptions>::read` — static member, src/libstore/serve-protocol.cc — maxSilentTime / buildTimeout / (≥2.2) maxLogSize / (≥2.3) nrRepeats + enforceDeterminism / (≥2.7) keepFailed.
- `ServeProto::Serialise<ServeProto::BuildOptions>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer.
- `ServeProto::Serialise<UnkeyedRealisation>::read` — static member, src/libstore/serve-protocol.cc — outPath + signatures; throws below 2.8.
- `ServeProto::Serialise<UnkeyedRealisation>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer; throws below 2.8.
- `ServeProto::Serialise<DrvOutput>::read` — static member, src/libstore/serve-protocol.cc — drvPath + outputName; throws below 2.8.
- `ServeProto::Serialise<DrvOutput>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer; throws below 2.8.
- `ServeProto::Serialise<Realisation>::read` — static member, src/libstore/serve-protocol.cc — DrvOutput id + UnkeyedRealisation body.
- `ServeProto::Serialise<Realisation>::write` — static member, src/libstore/serve-protocol.cc — symmetric writer; downcasts to `UnkeyedRealisation` for body.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libstore/include/nix/store/serve-protocol.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/serve-protocol.hh.

### Classes / structs / enums
- `ServeProto` — struct (acts as namespace; usable as template argument), src/libstore/include/nix/store/serve-protocol.hh.
- `ServeProto::Command` — `enum struct : uint64_t`, src/libstore/include/nix/store/serve-protocol.hh — wire commands: QueryValidPaths=1, QueryPathInfos=2, DumpStorePath=3, ImportPaths=4 (still used by Hydra, retained for back-compat), BuildPaths=6, QueryClosure=7, BuildDerivation=8, AddToStoreNar=9. (5 = removed `ExportPaths`.) NB: this is the serve-protocol analogue of `WorkerProto::Op`.
- `ServeProto::Version` — struct, src/libstore/include/nix/store/serve-protocol.hh — `unsigned int major; uint8_t minor;` with defaulted `<=>` (full ordering, no feature flags) and `toWire`/`fromWire`. Note: there is no `FeatureSet` here — this is the principal structural divergence from `WorkerProto::Version`.
- `ServeProto::ReadConn` — struct, src/libstore/include/nix/store/serve-protocol.hh — `Source & from; Version version;` (NB: stored by value, unlike worker-proto which stores a reference).
- `ServeProto::WriteConn` — struct, src/libstore/include/nix/store/serve-protocol.hh — `Sink & to; Version version;` (also by value).
- `ServeProto::BasicClientConnection` — struct (forward declaration here), src/libstore/include/nix/store/serve-protocol.hh.
- `ServeProto::BasicServerConnection` — struct (forward declaration here), src/libstore/include/nix/store/serve-protocol.hh.
- `ServeProto::Serialise<T>` — primary template (declared, undefined globally), src/libstore/include/nix/store/serve-protocol.hh — same trick as WorkerProto.
- `ServeProto::BuildOptions` — struct, src/libstore/include/nix/store/serve-protocol.hh — `time_t maxSilentTime; time_t buildTimeout; size_t maxLogSize; size_t nrRepeats; bool enforceDeterminism; bool keepFailed;` all defaulted to `-1` so older deserializers leave them untouched; defaulted `operator==`.

### Functions
- `ServeProto::Version::operator<=>` — defaulted total ordering, src/libstore/include/nix/store/serve-protocol.hh.
- `ServeProto::Version::toWire` — `constexpr` member, src/libstore/include/nix/store/serve-protocol.hh — `(major << 8) | minor`.
- `ServeProto::Version::fromWire` — `constexpr` static member, src/libstore/include/nix/store/serve-protocol.hh — inverse.
- `ServeProto::write<T>` — static template wrapper, src/libstore/include/nix/store/serve-protocol.hh — forwards to `Serialise<T>::write`.
- `ServeProto::BuildOptions::operator==` — defaulted, src/libstore/include/nix/store/serve-protocol.hh.
- `operator<<(Sink &, ServeProto::Command)` — free function (inline), src/libstore/include/nix/store/serve-protocol.hh — sends command as `uint64_t`.
- `operator<<(std::ostream &, ServeProto::Command)` — free function (inline), src/libstore/include/nix/store/serve-protocol.hh — debug printing.

### Type aliases
(none)

### Macros / globals
- `SERVE_MAGIC_1` (0x390c9deb), `SERVE_MAGIC_2` (0x5452eecb) — `#define`, src/libstore/include/nix/store/serve-protocol.hh — handshake magic constants.
- `SERVE_PROTOCOL_VERSION` ((2<<8)|8) — `#define`, src/libstore/include/nix/store/serve-protocol.hh — current wire version.
- `GET_PROTOCOL_MAJOR(x)` `((x) & 0xff00)` — `#define`, src/libstore/include/nix/store/serve-protocol.hh — identical name and definition to the macro in worker-protocol.hh (see Cross-file observations).
- `GET_PROTOCOL_MINOR(x)` `((x) & 0x00ff)` — `#define`, src/libstore/include/nix/store/serve-protocol.hh — same observation.
- `ServeProto::latest` — `static constexpr Version`, src/libstore/include/nix/store/serve-protocol.hh — `{2, 8}`.
- `DECLARE_SERVE_SERIALISER(T)` — `#define`, src/libstore/include/nix/store/serve-protocol.hh — emits read/write declarations for `ServeProto::Serialise<T>`.
- `COMMA_` — `#define ,` / `#undef`, src/libstore/include/nix/store/serve-protocol.hh — local helper.
- Serve-proto serialise specializations declared via `template<> DECLARE_SERVE_SERIALISER(...)` — BuildResult, DrvOutput, UnkeyedRealisation, Realisation, UnkeyedValidPathInfo, ServeProto::BuildOptions, std::vector<T>, std::set<T,Compare>, std::tuple<Ts...>, std::map<K,V,Compare>.

---

## File: src/libstore/include/nix/store/serve-protocol-impl.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/serve-protocol-impl.hh.

### Classes / structs / enums
- `ServeProto::Serialise<T>` — primary template definition (mirror of WorkerProto's), src/libstore/include/nix/store/serve-protocol-impl.hh — generic fallback that delegates to `CommonProto::Serialise<T>` by constructing `CommonProto::ReadConn`/`WriteConn` from the serve conn's `from`/`to`.

### Functions
- `ServeProto::Serialise<std::vector<T>>::{read,write}` — defined via `SERVE_USE_LENGTH_PREFIX_SERIALISER`, src/libstore/include/nix/store/serve-protocol-impl.hh.
- `ServeProto::Serialise<std::set<T,Compare>>::{read,write}` — same, src/libstore/include/nix/store/serve-protocol-impl.hh.
- `ServeProto::Serialise<std::tuple<Ts...>>::{read,write}` — same, src/libstore/include/nix/store/serve-protocol-impl.hh.
- `ServeProto::Serialise<std::map<K,V,Compare>>::{read,write}` — same, src/libstore/include/nix/store/serve-protocol-impl.hh.

### Type aliases
(none)

### Macros / globals
- `SERVE_USE_LENGTH_PREFIX_SERIALISER(TEMPLATE, T)` — `#define`, src/libstore/include/nix/store/serve-protocol-impl.hh — emits delegation to `LengthPrefixedProtoHelper<ServeProto, T>`. Structurally identical to `WORKER_USE_LENGTH_PREFIX_SERIALISER` and `COMMON_USE_LENGTH_PREFIX_SERIALISER`.
- `COMMA_` — `#define ,` / `#undef`, src/libstore/include/nix/store/serve-protocol-impl.hh — local helper for the `std::set` specialization.
- `SERVE_USE_LENGTH_PREFIX_SERIALISER_COMMA` — `#define ,`, src/libstore/include/nix/store/serve-protocol-impl.hh — used in the `std::map` specialization (no matching `#undef`).

---

## File: src/libstore/serve-protocol-connection.cc

### Namespaces
- `nix` — namespace, src/libstore/serve-protocol-connection.cc.

### Classes / structs / enums
(none defined here; method bodies only)

### Functions
- `ServeProto::BasicClientConnection::handshake` — static member, src/libstore/serve-protocol-connection.cc — sends SERVE_MAGIC_1 + version, validates SERVE_MAGIC_2, reads remote version, requires major=2 and minor ≥5; returns `min(remote, local)` (no feature exchange). Takes a `host` string for context in error messages.
- `ServeProto::BasicServerConnection::handshake` — static member, src/libstore/serve-protocol-connection.cc — server mirror; does not enforce a version-floor, returns `min(remote, local)`.
- `ServeProto::BasicClientConnection::queryValidPaths` — member, src/libstore/serve-protocol-connection.cc — sends `Command::QueryValidPaths` + `lock` bool + `maybeSubstitute` flag + paths, flushes, reads back `StorePathSet`.
- `ServeProto::BasicClientConnection::queryPathInfos` — member, src/libstore/serve-protocol-connection.cc — sends `Command::QueryPathInfos` + paths, then loops reading `(storePath, UnkeyedValidPathInfo)` pairs until empty path string sentinel; asserts each path was in the requested set.
- `ServeProto::BasicClientConnection::putBuildDerivationRequest` — member, src/libstore/serve-protocol-connection.cc — sends `Command::BuildDerivation` + drvPath + serialized derivation (`writeDerivation`) + BuildOptions; flushes.
- `ServeProto::BasicClientConnection::getBuildDerivationResponse` — member, src/libstore/serve-protocol-connection.cc — reads `BuildResult`.
- `ServeProto::BasicClientConnection::narFromPath` — member, src/libstore/serve-protocol-connection.cc — sends `Command::DumpStorePath` + path, flushes, hands `from` to the `receiveNar` callback.
- `ServeProto::BasicClientConnection::importPaths` — member, src/libstore/serve-protocol-connection.cc — sends `Command::ImportPaths`, calls callback to push NAR data, flushes, reads success int (must be 1) or throws.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libstore/include/nix/store/serve-protocol-connection.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/serve-protocol-connection.hh.

### Classes / structs / enums
- `ServeProto::BasicClientConnection` — struct (does **not** derive a shared `BasicConnection` base; serve-proto holds its own `FdSink to`, `FdSource from`, `ServeProto::Version remoteVersion` directly), src/libstore/include/nix/store/serve-protocol-connection.hh — provides implicit `operator ReadConn()` / `operator WriteConn()` and the client-side request helpers.
- `ServeProto::BasicServerConnection` — struct (no fields), src/libstore/include/nix/store/serve-protocol-connection.hh — only the static `handshake`.

### Functions
- `ServeProto::BasicClientConnection::handshake` — static declaration (takes additional `host` string), src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::operator ServeProto::ReadConn()` — implicit conversion, src/libstore/include/nix/store/serve-protocol-connection.hh — produces `{from, remoteVersion}`.
- `ServeProto::BasicClientConnection::operator ServeProto::WriteConn()` — implicit conversion, src/libstore/include/nix/store/serve-protocol-connection.hh — produces `{to, remoteVersion}`.
- `ServeProto::BasicClientConnection::queryValidPaths` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::queryPathInfos` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::putBuildDerivationRequest` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::getBuildDerivationResponse` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::narFromPath` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicClientConnection::importPaths` — member declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.
- `ServeProto::BasicServerConnection::handshake` — static declaration, src/libstore/include/nix/store/serve-protocol-connection.hh.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libstore/common-protocol.cc

### Namespaces
- `nix` — namespace, src/libstore/common-protocol.cc.

### Classes / structs / enums
(none defined here; method bodies only)

### Functions
- `CommonProto::Serialise<std::string>::read` — static member, src/libstore/common-protocol.cc — `readString(conn.from)`.
- `CommonProto::Serialise<std::string>::write` — static member, src/libstore/common-protocol.cc — `conn.to << str`.
- `CommonProto::Serialise<StorePath>::read` — static member, src/libstore/common-protocol.cc — `store.parseStorePath(readString(...))`.
- `CommonProto::Serialise<StorePath>::write` — static member, src/libstore/common-protocol.cc — `conn.to << store.printStorePath(...)`.
- `CommonProto::Serialise<ContentAddress>::read` — static member, src/libstore/common-protocol.cc — `ContentAddress::parse(readString(...))`.
- `CommonProto::Serialise<ContentAddress>::write` — static member, src/libstore/common-protocol.cc — `conn.to << renderContentAddress(...)`.
- `CommonProto::Serialise<std::optional<StorePath>>::read` — static member, src/libstore/common-protocol.cc — empty string sentinel = `nullopt`; otherwise `parseStorePath`.
- `CommonProto::Serialise<std::optional<StorePath>>::write` — static member, src/libstore/common-protocol.cc — emits empty string when absent; otherwise `printStorePath`.
- `CommonProto::Serialise<std::optional<ContentAddress>>::read` — static member, src/libstore/common-protocol.cc — `ContentAddress::parseOpt(readString(...))`.
- `CommonProto::Serialise<std::optional<ContentAddress>>::write` — static member, src/libstore/common-protocol.cc — emits empty string when absent; otherwise `renderContentAddress`.
- `CommonProto::Serialise<Signature>::read` — static member, src/libstore/common-protocol.cc — `Signature::parse(readString(...))`.
- `CommonProto::Serialise<Signature>::write` — static member, src/libstore/common-protocol.cc — `conn.to << sig.to_string()`.
- `CommonProto::Serialise<BuildResultStatus>::read` — static member, src/libstore/common-protocol.cc — reads a `uint8_t` wire value, validates against `std::size(buildResultStatusTable)`, returns the indexed `BuildResultStatus`.
- `CommonProto::Serialise<BuildResultStatus>::write` — static member, src/libstore/common-protocol.cc — converts `BuildResultFailureStatus::HashMismatch` → `OutputRejected` (older protocols don't know HashMismatch), then linearly searches `buildResultStatusTable` for the matching wire ordinal; calls `unreachable()` if not found.

### Type aliases
(none)

### Macros / globals
- `buildResultStatusTable` — `constexpr static BuildResultStatus[]` (file-scope), src/libstore/common-protocol.cc — wire-ordinal-indexed status table covering 15 entries: 0=BuildResultSuccessStatus::Built, 1=Substituted, 2=AlreadyValid, 3=BuildResultFailureStatus::PermanentFailure, 4=InputRejected, 5=OutputRejected, 6=TransientFailure, 7=CachedFailure, 8=TimedOut, 9=MiscFailure, 10=DependencyFailed, 11=LogLimitExceeded, 12=NotDeterministic, 13=BuildResultSuccessStatus::ResolvesToAlreadyValid, 14=BuildResultFailureStatus::NoSubstituters. `HashMismatch` is intentionally absent and rerouted on write.

---

## File: src/libstore/include/nix/store/common-protocol.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/common-protocol.hh.

### Classes / structs / enums
- `CommonProto` — struct (acts as namespace), src/libstore/include/nix/store/common-protocol.hh — shared serializers between worker and serve protocols.
- `CommonProto::ReadConn` — struct, src/libstore/include/nix/store/common-protocol.hh — `Source & from;` (NO version field — common protocol is version-independent).
- `CommonProto::WriteConn` — struct, src/libstore/include/nix/store/common-protocol.hh — `Sink & to;` (no version field).
- `CommonProto::Serialise<T>` — primary template (declared, undefined), src/libstore/include/nix/store/common-protocol.hh — must be specialised; no generic fallback definition (unlike WorkerProto/ServeProto).

### Functions
- `CommonProto::write<T>` — static template wrapper, src/libstore/include/nix/store/common-protocol.hh — forwards to `Serialise<T>::write`.

### Type aliases
- `BuildResultStatus` — `using BuildResultStatus = std::variant<BuildResultSuccessStatus, BuildResultFailureStatus>;`, src/libstore/include/nix/store/common-protocol.hh — used as a single wire-tag-domain by `Serialise<BuildResultStatus>` (the success and failure tag spaces are disjoint).

### Macros / globals
- `DECLARE_COMMON_SERIALISER(T)` — `#define`, src/libstore/include/nix/store/common-protocol.hh — emits read/write declarations for `CommonProto::Serialise<T>`.
- `COMMA_` — `#define ,` / `#undef`, src/libstore/include/nix/store/common-protocol.hh — local helper. Note: there is also a stray `#undef COMMA_` at the very end of the file with no matching `#define`.
- Common-proto serialise specializations declared via `template<> DECLARE_COMMON_SERIALISER(...)` — std::string, StorePath, ContentAddress, DrvOutput, Realisation, Signature, std::vector<T>, std::set<T,Compare>, std::tuple<Ts...>, std::map<K,V,Compare>, std::optional<StorePath>, std::optional<ContentAddress>, BuildResultStatus.

---

## File: src/libstore/include/nix/store/common-protocol-impl.hh

### Namespaces
- `nix` — namespace, src/libstore/include/nix/store/common-protocol-impl.hh.

### Classes / structs / enums
(none)

### Functions
- `CommonProto::Serialise<std::vector<T>>::{read,write}` — defined via `COMMON_USE_LENGTH_PREFIX_SERIALISER`, src/libstore/include/nix/store/common-protocol-impl.hh.
- `CommonProto::Serialise<std::set<T,Compare>>::{read,write}` — same, src/libstore/include/nix/store/common-protocol-impl.hh.
- `CommonProto::Serialise<std::tuple<Ts...>>::{read,write}` — same, src/libstore/include/nix/store/common-protocol-impl.hh.
- `CommonProto::Serialise<std::map<K,V,Compare>>::{read,write}` — same, src/libstore/include/nix/store/common-protocol-impl.hh.

### Type aliases
(none)

### Macros / globals
- `COMMON_USE_LENGTH_PREFIX_SERIALISER(TEMPLATE, T)` — `#define`, src/libstore/include/nix/store/common-protocol-impl.hh — emits delegation to `LengthPrefixedProtoHelper<CommonProto, T>`. Structurally identical to `WORKER_USE_LENGTH_PREFIX_SERIALISER` and `SERVE_USE_LENGTH_PREFIX_SERIALISER`.
- `COMMA_` — `#define ,` / `#undef`, src/libstore/include/nix/store/common-protocol-impl.hh — local helper.

---

## File: src/libstore/daemon.cc

### Namespaces
- `nix::daemon` — namespace, src/libstore/daemon.cc — encloses every definition in this translation unit.

### Classes / structs / enums
- `TunnelLogger` — struct (file-scope) deriving `Logger`, src/libstore/daemon.cc — forwards log lines to the client over the wire when `canSendStderr` is true; otherwise queues them. Holds `FdSink & to`, `Sync<State> state_`, `WorkerProto::Version clientVersion`.
- `TunnelLogger::State` — nested struct, src/libstore/daemon.cc — `bool canSendStderr = false; std::vector<std::string> pendingMsgs;`.
- `TunnelSource` — struct (file-scope) deriving `BufferedSource`, src/libstore/daemon.cc — pulls bytes from the client by sending STDERR_READ + length and reading the framed reply via `readString(data, len, from)`. Holds `Source & from; BufferedSink & to;`.
- `ClientSettings` — struct (file-scope), src/libstore/daemon.cc — captures the SetOptions request payload (`keepFailed`, `keepGoing`, `tryFallback`, `verbosity`, `maxBuildJobs`, `maxSilentTime`, `verboseBuild`, `buildCores`, `useSubstitutes`, `overrides` StringMap) and applies it to global settings.

### Functions
- `operator<<(Sink &, const Logger::Fields &)` — free function (in `nix::daemon`), src/libstore/daemon.cc — serialises a `Logger::Fields` vector with size prefix and per-field type tag (`tInt` → `f.i`, `tString` → `f.s`); calls `unreachable()` on unknown field type.
- `TunnelLogger::TunnelLogger` — constructor, src/libstore/daemon.cc — captures `FdSink &` and `WorkerProto::Version`.
- `TunnelLogger::enqueueMsg` — member, src/libstore/daemon.cc — flushes immediately (with state lock) when `canSendStderr`; otherwise pushes onto `pendingMsgs`. Resets `canSendStderr` and rethrows if write to the wire fails.
- `TunnelLogger::log` — `void log(Verbosity lvl, std::string_view s) override`, src/libstore/daemon.cc — STDERR_NEXT-prefixed line; respects `verbosity` filter.
- `TunnelLogger::logEI` — `void logEI(const ErrorInfo & ei) override`, src/libstore/daemon.cc — STDERR_NEXT-prefixed pretty-printed `ErrorInfo` via `showErrorInfo`.
- `TunnelLogger::startWork` — member, src/libstore/daemon.cc — drains pending messages and starts allowing live forwarding.
- `TunnelLogger::stopWork` — member (`const Error * ex = nullptr`), src/libstore/daemon.cc — sends STDERR_LAST when `ex == nullptr`; otherwise STDERR_ERROR followed by a serialized `Error` (≥1.26) or `(what(), info().status)` pair.
- `TunnelLogger::startActivity` — `override`, src/libstore/daemon.cc — emits STDERR_START_ACTIVITY (≥1.20) or falls back to a log-line ("...").
- `TunnelLogger::stopActivity` — `override`, src/libstore/daemon.cc — emits STDERR_STOP_ACTIVITY (no-op below 1.20).
- `TunnelLogger::result` — `override`, src/libstore/daemon.cc — emits STDERR_RESULT (no-op below 1.20).
- `TunnelSource::TunnelSource` — constructor, src/libstore/daemon.cc.
- `TunnelSource::readUnbuffered` — `override`, src/libstore/daemon.cc — sends STDERR_READ + len, flushes, then reads bytes via `readString(data, len, from)`; throws `EndOfFile` on zero bytes.
- `ClientSettings::apply` — member, src/libstore/daemon.cc — copies fields into `settings`, `settings.getWorkerSettings()`, `settings.getLocalSettings()` (for `buildCores`), and the global `nix::verbosity`; iterates `overrides` and applies each one with trust-aware filtering. Includes a special-case `setSubstituters` lambda that only allows trusted substituters and warns otherwise; ignores a few keys (`ssh-auth-sock`, the experimental-features setting, `plugin-files`); allows untrusted clients to set `build-timeout`/`max-silent-time`/`build-poll-interval`/`connect-timeout` and to clear `builders`.
- `performOp` — file-static free function in `nix::daemon`, src/libstore/daemon.cc — the master message-dispatch switch. Takes `(TunnelLogger *, ref<Store>, TrustedFlag, RecursiveFlag, WorkerProto::BasicServerConnection &, WorkerProto::Op)`. Handles every `WorkerProto::Op` case (see below).
- `processConnection` — free function in `nix::daemon`, src/libstore/daemon.cc — entry point used by nix-daemon: sets up `MonitorFdHup` (non-Windows, non-recursive only), installs an interrupt callback that calls `RemoteStore::shutdownConnections` to break circular waits, performs the worker-protocol handshake via `BasicServerConnection::handshake` adding `featureDisableSetOptions` to the local feature set when recursive, enforces `protoVersion.number >= WorkerProto::minimum.number`, installs `TunnelLogger` (replacing the global `logger` when not recursive) and applies any `--log-format=internal-json` override via `applyJSONLogger`, runs `postHandshake` (with `daemonNixVersion=nixVersion` and `remoteTrustsUs` derived from the `trusted` flag and `store->isTrustedClient()`), then loops reading ops and calling `performOp`. Catches `Error`/`std::bad_alloc`/`std::exception` from `performOp` to send back via `tunnelLogger->stopWork(&ex)`. Final `Finally` resets `setInterrupted(false)` and prints the operation count.

### Type aliases
(none)

### Macros / globals
(none)

### Op-handler dispatch table (cases handled by `performOp`)
For cross-protocol comparison, here is every `WorkerProto::Op` case the daemon answers:
- `IsValidPath` — reads StorePath, replies with `store->isValidPath(path)` as bool.
- `QueryValidPaths` — reads StorePathSet; (≥1.27) reads optional `substitute` int; if substitute, calls `store->substitutePaths(paths)`; replies with `store->queryValidPaths(paths, substitute)`.
- `QuerySubstitutablePaths` — reads StorePathSet, replies with `store->querySubstitutablePaths`.
- `QueryReferrers` / `QueryValidDerivers` / `QueryDerivationOutputs` — three opcodes share a single switch arm that picks `queryReferrers`/`queryValidDerivers`/`queryDerivationOutputs` based on the op.
- `QueryDerivationOutputNames` — replies with `store->readDerivation(path).outputNames()` as a Strings (raw stream, not Serialise).
- `QueryDerivationOutputMap` — replies with `store->queryPartialDerivationOutputMap`.
- `QueryDeriver` — returns `queryPathInfo(path)->deriver` via the optional<StorePath> serializer.
- `QueryPathFromHashPart` — reads hashPart string, replies with `store->queryPathFromHashPart`.
- `AddToStore` — two protocol paths: ≥1.25 reads name + `ContentAddressMethod` string + refs + repair, then drains a `FramedSource` into `addToStoreFromDump` and writes the resulting `ValidPathInfo`; legacy path (<1.25) reads baseName/fixed/recursive/hashAlgo, parses NAR via `TeeSource`+`NullFileSystemObjectSink`, and writes back the resulting StorePath.
- `AddMultipleToStore` — reads `repair`/`dontCheckSigs` (clamps `dontCheckSigs` to false for untrusted), drains a `FramedSource` for the count and per-info NAR (each info read with a hardcoded `Version{1,16}` ReadConn), wraps each NAR in `EnsureRead`, calls `addToStore`.
- `AddTextToStore` — reads suffix + payload + refs; calls `addToStoreFromDump(StringSource, ..., Raw::Text, SHA256, refs, NoRepair)`. Obsolete since 1.25.
- `BuildPaths` — reads DerivedPaths + BuildMode; rejects `bmRepair` for untrusted clients; calls `buildPaths`; replies with int 1.
- `BuildPathsWithResults` — same input, same trust check; replies with `buildPathsWithResults` results vector.
- `BuildDerivation` — reads drvPath + derivation (via `readDerivation`) + BuildMode; non-CA derivations require trust; for untrusted clients with CA derivations, recomputes drvPath via `writeDerivation`; replies with `buildDerivation` BuildResult.
- `EnsurePath` — reads StorePath, calls `ensurePath`, replies with int 1.
- `AddTempRoot` — reads StorePath, calls `addTempRoot`, replies with int 1.
- `AddPermRoot` — requires trust; reads StorePath + raw `gcRoot` path; downcasts to `LocalFSStore` and calls `addPermRoot`; replies with the gcRoot string.
- `AddIndirectRoot` — reads raw path, downcasts to `IndirectRootStore` and calls `addIndirectRoot`; replies with int 1.
- `SyncWithGC` (obsolete) — start/stopWork no-op, replies with int 1.
- `FindRoots` — downcasts to `GcStore` and calls `findRoots(!trusted)`; replies with total count then `(link, target)` pairs.
- `CollectGarbage` — reads `GCAction`, then either `GCPaths` (with `featureDeleteDeadSpecificReferrers`) or legacy `StorePathSet` plus inferred `WholeStore`/`SpecificPaths`; reads `ignoreLiveness` + `maxFreed` + three obsolete ints; rejects `gcDeleteDead` of specific paths without the feature; throws if `ignoreLiveness`; replies with `(paths, bytesFreed, 0)`.
- `SetOptions` — reads keepFailed/keepGoing/tryFallback/verbosity/maxBuildJobs/maxSilentTime + obsolete useBuildHook + verboseBuild + obsolete logType/printBuildTrace + buildCores/useSubstitutes + overrides count and pairs; populates a `ClientSettings` and calls `clientSettings.apply(trusted)` (skipped in recursive mode).
- `QuerySubstitutablePathInfo` — reads StorePath, calls `querySubstitutablePathInfos({{path, nullopt}}, infos)`; replies 0 if absent or 1 + deriver/refs/downloadSize/narSize.
- `QuerySubstitutablePathInfos` — bulk version; ≥1.22 reads `StorePathCAMap`, below reads plain `StorePathSet` and converts; replies with size + per-entry `(path, deriver, refs, downloadSize, narSize)`.
- `QueryAllValidPaths` — replies with `queryAllValidPaths`.
- `QueryPathInfo` — reads StorePath, replies with `0` (absent) or `1 + UnkeyedValidPathInfo`.
- `OptimiseStore` — calls `optimiseStore`, replies with int 1.
- `VerifyStore` — reads `(checkContents, repair)`; `repair` requires trust; replies with bool errors result.
- `AddSignatures` — reads StorePath + signatures set, calls `addSignatures`, replies with int 1.
- `NarFromPath` — reads StorePath; calls `narFromPath(path, conn.to)` (note: start/stopWork is empty around this).
- `AddToStoreNar` — composes `ValidPathInfo` from the wire (path, deriver, narHash SHA256, refs, regTime, narSize, ultimate, sigs, ca, repair, dontCheckSigs); ≥1.23 uses `FramedSource`; ≥1.21 uses `TunnelSource`; below 1.21 parses NAR via `TeeSource` and replays via `StringSource`; clamps `dontCheckSigs`/`ultimate` for untrusted clients; calls `addToStore` wrapped in `EnsureRead`.
- `QueryMissing` — reads DerivedPaths; replies with `(willBuild, willSubstitute, unknown, downloadSize, narSize)`.
- `RegisterDrvOutput` — reads `Realisation`, calls `registerDrvOutput`.
- `QueryRealisation` — reads `DrvOutput`, asserts `featureRealisationWithPath`, replies with `optional<UnkeyedRealisation>`.
- `AddBuildLog` — requires trust; reads StorePath; downcasts to `LogStore`; drains `FramedSource` into a string and calls `addBuildLog`; replies with int 1.
- default — `throw Error("invalid operation %1%", op)`.

---

## File: src/libstore/include/nix/store/daemon.hh

### Namespaces
- `nix::daemon` — namespace, src/libstore/include/nix/store/daemon.hh.

### Classes / structs / enums
- `RecursiveFlag` — `enum RecursiveFlag : bool { NotRecursive = false, Recursive = true }`, src/libstore/include/nix/store/daemon.hh — type-tag for whether the daemon is running inside a recursive-nix invocation.

### Functions
- `processConnection` — free function declaration `(ref<Store>, FdSource &&, FdSink &&, TrustedFlag, RecursiveFlag)`, src/libstore/include/nix/store/daemon.hh — public entry point implemented in daemon.cc.

### Type aliases
(none)

### Macros / globals
(none)

---

## Cross-file observations

The three protocol families (`CommonProto`, `WorkerProto`, `ServeProto`) are deliberately laid out as parallel structures. The duplication is largely systematic and should be straightforward to refactor.

1. **Trinity of namespace-shaped struct types.**
   `CommonProto`, `WorkerProto`, `ServeProto` are each implemented as a `struct` so they can be passed as a template argument (in particular to `LengthPrefixedProtoHelper<Proto, T>`). Each one provides:
   - `ReadConn { Source & from; ... }` and `WriteConn { Sink & to; ... }` (worker stores `const Version &`; serve stores `Version` by value; common has no version field).
   - A primary `template<typename T> struct Serialise;` that is *declared but undefined* at the global level, so missing serializers fail at compile time, not link time.
   - A helper `static void write(...)` function template that forwards to `Serialise<T>::write` (identical body in all three).

2. **Identical declaration macros.**
   `DECLARE_COMMON_SERIALISER`, `DECLARE_WORKER_SERIALISER`, `DECLARE_SERVE_SERIALISER` differ only in the namespace prefix on `Serialise<T>` and (cosmetically) in the parameter name (`str` vs `t`). Same for the `*_USE_LENGTH_PREFIX_SERIALISER` macros that auto-define container serializers via `LengthPrefixedProtoHelper`.

3. **Three-tier delegation.**
   The fallback `WorkerProto::Serialise<T>` and `ServeProto::Serialise<T>` (defined in their `*-impl.hh` files) both delegate to `CommonProto::Serialise<T>` by stripping the version off the conn before forwarding. So adding a new common-proto specialization is automatically usable from both worker and serve callers; conversely, anything truly common (string/StorePath/ContentAddress/DrvOutput/Realisation/Signature/optional<StorePath>/optional<ContentAddress>/BuildResultStatus, plus all the `vector/set/tuple/map` containers) lives in `CommonProto`.

4. **`Version` divergence.**
   `WorkerProto::Version` is a struct of `{Number {major, minor}, FeatureSet}` with **partial** ordering (subset relation on features), and stores a `Version &` by reference in the connection types. `WorkerProto::Version::Number` itself has total ordering via defaulted `operator<=>`. `ServeProto::Version` is `{major, minor}` directly with **default** total ordering, stored by value, no feature exchange. The wire-format helpers `toWire`/`fromWire` are duplicated near-verbatim between the two (`WorkerProto::Version::Number` vs `ServeProto::Version`).

5. **Parallel `Op` / `Command` enums.**
   `WorkerProto::Op` (37 live cases handled by `performOp`) and `ServeProto::Command` (8 live cases) are both `enum struct : uint64_t` with identical surrounding `operator<<` overloads (Sink and ostream). The serve-protocol commands are a subset of what the worker protocol does, but they have *separate* numeric spaces and *separate* serializers.

6. **Duplicated handshake macros.**
   `GET_PROTOCOL_MAJOR(x)` / `GET_PROTOCOL_MINOR(x)` are `#define`d in both `worker-protocol.hh` and `serve-protocol.hh` with identical bodies (`(x) & 0xff00` / `(x) & 0x00ff`). Since these are bare textual macros, including both headers in the same TU works only because the second `#define` produces an identical token sequence (the C standard treats this as a benign redefinition). Magic numbers `WORKER_MAGIC_1/2` vs `SERVE_MAGIC_1/2` are independent constants.

7. **Duplicated handshake logic.**
   `WorkerProto::BasicClientConnection::handshake` (worker-protocol-connection.cc) and `ServeProto::BasicClientConnection::handshake` (serve-protocol-connection.cc) follow the same shape: send magic-1, read magic-2, exchange version numbers, take the min. Worker additionally exchanges a `FeatureSet` post-version-check (≥1.38) and intersects them via a private `intersectFeatures` helper; serve has no such step. Worker enforces `daemonVersion >= 1.10` and same major; serve enforces `major == 2` and `minor >= 5`. The server-side mirrors are likewise parallel.

8. **Duplicated `BuildResult` serializer logic.**
   `WorkerProto::Serialise<BuildResult>` (worker-protocol.cc) and `ServeProto::Serialise<BuildResult>` (serve-protocol.cc) are near-identical: same status/errorMsg/timing/builtOutputs sequence, with version cutoffs differing only in the literal numeric thresholds (worker uses `{1,29}`/`{1,37}`/`featureRealisationWithPath`/`{1,28}`; serve uses `{2,3}`/`{2,8}`/`{2,6}`). The `common = [&](errorMsg, isNonDeterministic, builtOutputs) { ... }` lambda used to share the success/failure path is duplicated almost verbatim between the two TUs. Worker has the extra cpuUser/cpuSystem field and the `featureRealisationWithPath` feature gate; serve has no cpu-timing branch and unconditionally uses the new format above 2.8. This is the single biggest refactoring target identified in this shard.

9. **Duplicated `UnkeyedValidPathInfo` serializer logic.**
   Both protocols hand-roll the deriver/refs/narHash/narSize/sigs/ca format with cosmetic differences:
   - Worker reads/writes the deriver via `Serialise<std::optional<StorePath>>`; serve uses an empty-string sentinel inline without going through the optional serializer.
   - Worker uses `HashFormat::Base16` (no prefix) for narHash; serve uses `Hash::parseAnyPrefixed`/`HashFormat::Nix32` (with prefix).
   - Serve emits `narSize` twice (the first as the obsolete "downloadSize" "lying a little"); worker emits only `narSize`.
   - Worker emits `ultimate` flag; serve does not.
   - Both gate sigs/ca on a per-protocol minimum version (worker ≥1.16, serve ≥2.4).

10. **Identical `DrvOutput`/`UnkeyedRealisation`/`Realisation` serializers.**
    Worker and Serve protocols both implement these three types exactly the same way (drvPath + outputName for DrvOutput; outPath + signatures for UnkeyedRealisation; DrvOutput id + UnkeyedRealisation body for Realisation); only the version-gate differs (`featureRealisationWithPath` for worker, `< 2.8` for serve). These are obvious candidates for promotion to `CommonProto`, except that the version-checks themselves are protocol-specific.

11. **`BasicConnection` design diverges.**
    Worker has a shared `BasicConnection` base struct that holds `to`/`from`/`protoVersion` and is inherited by `BasicClientConnection`/`BasicServerConnection`, with implicit conversions to `Read/WriteConn`. Worker's client also has a virtual destructor and pure-virtual `closeWrite()`, plus the stderr-dispatch loop (`processStderr*`). Serve has no such shared base: `BasicClientConnection` holds its own `to`/`from`/`remoteVersion` fields with the implicit conversions, and `BasicServerConnection` is essentially just a static `handshake` (no state).

12. **`ClientHandshakeInfo` and `BuildOptions` are protocol-specific.**
    `WorkerProto::ClientHandshakeInfo` (`daemonNixVersion`, `remoteTrustsUs`) and `ServeProto::BuildOptions` (`maxSilentTime`/`buildTimeout`/`maxLogSize`/`nrRepeats`/`enforceDeterminism`/`keepFailed`) are not paralleled in the other protocol. Serve has no post-handshake step; worker's post-handshake reads obsolete CPU-affinity / reserveSpace bytes and exchanges `ClientHandshakeInfo`.

13. **`buildResultStatusTable` is the canonical wire-tag mapping.**
    Defined once in common-protocol.cc and shared by both `WorkerProto::Serialise<BuildResult>` and `ServeProto::Serialise<BuildResult>` via `Serialise<BuildResultStatus>`. The `HashMismatch` → `OutputRejected` redirect on write lives only at the common-protocol layer, so neither worker nor serve has to know about it explicitly. The table covers 15 entries (0..14); `HashMismatch` is the only `BuildResultFailureStatus` not present in the wire format.

14. **`STDERR_*` and `GET_PROTOCOL_*` macros live only in worker, not in serve or common.**
    `STDERR_*` message tags (NEXT/READ/WRITE/LAST/ERROR/START_ACTIVITY/STOP_ACTIVITY/RESULT) are defined only in `worker-protocol.hh`. Both `daemon.cc` (server-side) and `worker-protocol-connection.cc` (client-side) consume them. Serve protocol has no out-of-band stderr framing — its commands are strictly request/response.

15. **Macro hygiene caveats.**
    All three `*_USE_LENGTH_PREFIX_SERIALISER_COMMA` helpers (`WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA` and `SERVE_USE_LENGTH_PREFIX_SERIALISER_COMMA`) are `#define`d but never `#undef`'d in their respective impl headers, leaking into translation units that include them. The `COMMA_` helpers in the protocol headers and impl headers do `#undef` consistently, except for a stray bare `#undef COMMA_` at the end of `common-protocol.hh` with no matching `#define` in scope.
