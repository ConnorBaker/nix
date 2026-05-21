# Inventory — Shard 10: libstore build (verified)

## File: src/libstore/build/build-log.cc

### Namespaces
- `nix` — out-of-line definitions for `BuildLog`.

### Classes / structs / enums
(none)

### Functions
- `BuildLog::BuildLog(size_t maxTailLines, std::unique_ptr<Activity> act)` — kind: ctor, purpose: store `maxTailLines` and take ownership of the build `Activity`.
- `BuildLog::operator()(std::string_view data)` — kind: member function (override of `Sink::operator()`), purpose: process raw bytes character-by-character, resetting `currentLogLinePos` on `\r`, calling `flushLine` on `\n`, otherwise appending to `currentLogLine`.
- `BuildLog::flush()` — kind: member function, purpose: flush the trailing partial line if non-empty.
- `BuildLog::flushLine()` — kind: private member function, purpose: truncate `currentLogLine` to `currentLogLinePos`, dispatch to `handleJSONLogMessage` or otherwise emit `resBuildLogLine` to the activity and append to `logTail`, popping the front when `logTail` exceeds `maxTailLines`; clears the line buffer.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/build-log.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `BuildLog` (struct, inherits `Sink`) — purpose: line-buffer build output, retain a tail of recent lines, parse JSON activity messages. Private fields: `maxTailLines`, `logTail` (`std::list<std::string>`), `currentLogLine`, `currentLogLinePos = 0`. Public fields: `act` (`std::unique_ptr<Activity>`), `builderActivities` (`std::map<ActivityId, Activity>`). Members: ctor, `operator()` (override of `Sink::operator()`), `flush()`, inline `getTail() const` returning `const std::list<std::string> &`, inline `hasPartialLine() const`, private `flushLine()`.

### Functions
(only the inline accessors `getTail`, `hasPartialLine` declared in-class)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-builder.cc

### Namespaces
- `nlohmann` — JSON serialiser specialisation namespace (with `using namespace nix;` inside).

### Classes / structs / enums
(none)

### Functions
- `nlohmann::adl_serializer<ExternalBuilder>::from_json(const json &)` — kind: free function in serializer, purpose: parse JSON object into `ExternalBuilder` value (`systems`, `program`, `args`).
- `nlohmann::adl_serializer<ExternalBuilder>::to_json(json &, const ExternalBuilder &)` — kind: free function in serializer, purpose: serialise `ExternalBuilder` to JSON object.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-builder.hh

### Namespaces
- `nix` — declares public `DerivationBuilder` interface and helpers.

### Classes / structs / enums
- `BuilderFailureError` (struct, final, inherits `CloneableError<BuilderFailureError, BuildError>`) — purpose: denote a build failure stemming from a builder process exiting with a failing status. Fields: `builderStatus` (`int`), `extraMsgAfter` (`std::string`). Single ctor `(BuildResult::Failure::Status, int, std::string)`.
- `ChrootPath` (struct) — purpose: describe a path bind-mounted into the chroot. Fields: `source` (`std::filesystem::path`), `optional` (`bool`, default `false`).
- `DerivationBuilderParams` (struct) — purpose: bundle the (mostly const-reference) parameters for a derivation builder. Fields: `drvPath` (`const StorePath &`), `buildResult` (`BuildResult &`), `drv` (`const BasicDerivation &`), `drvOptions` (`const DerivationOptions<StorePath> &`), `inputPaths` (`const StorePathSet &`), `initialOutputs` (`const std::map<std::string, InitialOutput>`), `buildMode` (`const BuildMode &`), `defaultPathsInChroot` (`PathsInChroot`), `systemFeatures` (`StringSet`), `desugaredEnv` (`DesugaredEnv`).
- `DerivationBuilderCallbacks` (struct, abstract) — purpose: pure-virtual hooks for log-file open/close and child-termination notifications. Members: virtual dtor, pure virtual `openLogFile()`, `closeLogFile()`, `childTerminated()`.
- `DerivationBuilder` (struct, inherits `RestrictionContext`) — purpose: abstract local builder interface. Public field: `builderOut` (`AutoCloseFD`, master pseudo-tty). Members: defaulted ctor, virtual dtor, pure virtual `startBuild() -> std::optional<Descriptor>`, `unprepareBuild() -> SingleDrvOutputs`, `killChild() -> bool`.
- `ExternalBuilder` (struct) — purpose: describe an external derivation builder configuration. Fields: `systems` (`StringSet`), `program` (`std::filesystem::path`), `args` (`std::vector<std::string>`).
- `DerivationBuilderDeleter` (struct) — purpose: custom deleter that calls `cleanupOnDestruction` on `DerivationBuilderImpl`. Member: `operator()(DerivationBuilder *) noexcept`.

### Functions
- `to_json(nlohmann::json &, const ChrootPath &)` — declaration, purpose: serialise a `ChrootPath`.
- `from_json(const nlohmann::json &, ChrootPath &)` — declaration, purpose: parse a `ChrootPath`.
- `preserveDeathSignal(fun<void()> setCredentials)` — declaration, purpose: invoke a credential-changing callback while preserving the parent-death signal (Linux-only behaviour, no-op elsewhere).
- `makeDerivationBuilder(LocalStore &, std::unique_ptr<DerivationBuilderCallbacks>, DerivationBuilderParams) -> DerivationBuilderUnique` — declaration (gated by `#ifndef _WIN32`), purpose: factory selecting platform-specific builder.
- `makeExternalDerivationBuilder(LocalStore &, std::unique_ptr<DerivationBuilderCallbacks>, DerivationBuilderParams, const ExternalBuilder &) -> DerivationBuilderUnique` — declaration (gated by `#ifndef _WIN32`), purpose: factory for external/handler-driven builders.

### Type aliases
- `PathsInChroot = std::map<std::filesystem::path, ChrootPath>` — typedef.
- `DerivationBuilderUnique = std::unique_ptr<DerivationBuilder, DerivationBuilderDeleter>` — using.

### Macros / globals
- `JSON_IMPL(nix::ExternalBuilder)` — macro invocation declaring the JSON serialiser specialisation for `ExternalBuilder`.

## File: src/libstore/build/derivation-building-goal.cc

### Namespaces
- `nix` — implements `DerivationBuildingGoal` plus several private helpers.

### Classes / structs / enums
- `LogSink` (struct, file-local, inherits `Sink`) — purpose: capture lines from the post-build hook and emit `resPostBuildLogLine` activity results. Fields: `act` (`Activity &`), `currentLine` (`std::string`). Members: ctor, `operator()` (override), `flushLine`, dtor (flushes any trailing line with a synthetic `\n`).
- `PostBuildHookState` (struct, file-local) — purpose: bundle the resources of a running post-build hook. Fields: `hook` (`const std::string`), `act` (`Activity`), `sink` (`std::unique_ptr<LogSink>`), `out` (`std::unique_ptr<Pipe>`), `pid` (`Pid`). Members: ctor (creates the pipe and the sink), `complete()` (waits on `pid`, throws on non-zero status).
- `LogFile` (struct, file-local, RAII) — purpose: own the on-disk build log fd plus its sinks. Fields: `fd` (`AutoCloseFD`), `fileSink`, `sink` (both `std::shared_ptr<BufferedSink>`). Members: ctor (opens the log file with optional bzip2 compression), dtor (finishes the compression sink and flushes the file sink).
- `LocalBuildRejection` (struct, file-local) — purpose: encode the reasons we cannot build locally for `tryToBuild`. Field: `maxJobsZero` (`bool`, default `false`). Nested types: `NoLocalStore` (empty tag struct); `WrongLocalStore` containing template `Pair<T>` with `derivation` and `localStore` plus `std::optional<Pair<std::string>> badPlatform`, `std::optional<Pair<StringSet>> missingFeatures`. Field: `rejection` (`std::variant<NoLocalStore, WrongLocalStore>`).
- `DerivationBuildingGoalCallbacks` (struct, scoped inside `buildLocally`, inherits `DerivationBuilderCallbacks`) — purpose: adapter that delegates `childTerminated` to `worker.childTerminated(&goal, JobCategory::Build)` and `openLogFile`/`closeLogFile` to captured `fun<void()>` callbacks. Fields: `goal` (`DerivationBuildingGoal &`), `openLogFileFn`, `closeLogFileFn`. Ctor and overrides for the three pure virtuals.

### Functions
- `DerivationBuildingGoal::DerivationBuildingGoal(const StorePath &, ref<const Derivation>, Worker &, BuildMode, bool storeDerivation)` — kind: ctor, purpose: register goal with worker, install temp-root for the drv path, kick off coroutine `gaveUpOnSubstitution(storeDerivation)`.
- `DerivationBuildingGoal::~DerivationBuildingGoal` — defaulted dtor.
- `DerivationBuildingGoal::key()` — kind: override, purpose: return scheduling key (`"dd$..."`).
- `showKnownOutputs(const StoreDirConfig &, const Derivation &) -> std::string` — kind: free function, purpose: format the list of known output paths for error messages.
- `static std::unique_ptr<PostBuildHookState> runPostBuildHook(...)` — kind: forward-declaration (file-local static), purpose: spawn the post-build hook process and return a `PostBuildHookState` (defined later in the file).
- `static BuildError reject(const LocalBuildRejection &, std::string_view thingCannotBuild)` — kind: file-local static, purpose: build a `BuildError` describing why a local build is impossible (no local store / wrong platform / missing features / `max-jobs=0`), with an aarch64-darwin Rosetta hint.
- `DerivationBuildingGoal::gaveUpOnSubstitution(bool storeDerivation)` — kind: coroutine `Goal::Co`, purpose: copy inputs from eval store to build store, schedule path substitutions for missing input sources, then yield to `tryToBuild` after computing the FS closure of all input paths.
- `DerivationBuildingGoal::tryToBuild(StorePathSet inputPaths)` — kind: coroutine, purpose: parse drv options, classify local-build feasibility (`LocalBuildCapability` vs `LocalBuildRejection`), define lambda coroutines `acquireResources`, `tryHookLoop`, `tryBuildLocally`, then dispatch based on `buildMode`/`preferLocalBuild` to `buildWithHook` or `buildLocally` (or fail via `reject`).
- `DerivationBuildingGoal::buildWithHook(StorePathSet, std::map<std::string, InitialOutput>, DerivationOptions<StorePath>, PathLocks)` — kind: coroutine, purpose: drive the build via the existing `HookInstance`, demux logger/hook channels, parse JSON activity messages (including `setPhase` re-emission to log file), run post-build hook, register outputs.
- `DerivationBuildingGoal::buildLocally(LocalBuildCapability, StorePathSet, std::map<std::string, InitialOutput>, DerivationOptions<StorePath>, PathLocks)` — kind: coroutine, purpose: instantiate platform-specific builder via `makeDerivationBuilder` or `makeExternalDerivationBuilder`, drive its `startBuild`/`unprepareBuild`, run post-build hook on success.
- `static std::unique_ptr<PostBuildHookState> runPostBuildHook(const WorkerSettings &, const StoreDirConfig &, Logger &, const StorePath & drvPath, const StorePathSet & outputPaths)` — kind: file-local static, purpose: fork the post-build hook with `DRV_PATH`, `OUT_PATHS`, `NIX_CONFIG` environment, return its `PostBuildHookState`. Throws `UnimplementedError` on Windows.
- `DerivationBuildingGoal::fixupBuilderFailureErrorMessage(BuilderFailureError, BuildLog &)` — kind: member function, purpose: format the human-readable failure message (last log lines + `nix log` hint) and wrap into a fresh `BuildError`.
- `DerivationBuildingGoal::tryBuildHook(const DerivationOptions<StorePath> &)` — kind: member function (returns `HookReply`), purpose: ask the hook process whether it accepts/declines/postpones the build; returns `rpDecline` on Windows.
- `LogFile::LogFile(Store &, const StorePath &, const LogFileSettings &)` / `LogFile::~LogFile()` — kind: ctor/dtor, purpose: open/close the log file with optional bzip2 compression; ctor is a no-op when `keepLog` is false.
- `DerivationBuildingGoal::doneFailureLogTooLong(BuildLog &)` — kind: member function, purpose: emit `BuildResult::Failure::LogLimitExceeded`.
- `DerivationBuildingGoal::queryPartialDerivationOutputMap()` — kind: member function, purpose: lookup output paths from store (via eval store or build store) or fall back to in-memory derivation outputs.
- `DerivationBuildingGoal::checkPathValidity(std::map<std::string, InitialOutput> &)` — kind: member function, purpose: refresh `initialOutputs` from store contents and produce `(allValid, validOutputs)`. Also registers realisations for CA derivations when needed.
- `DerivationBuildingGoal::doneSuccess(BuildResult::Success::Status, SingleDrvOutputs)` / `DerivationBuildingGoal::doneFailure(BuildError)` — kind: member functions, purpose: book-keeping (`mcRunningBuilds.reset()`, `worker.doneBuilds++`/`failedBuilds++`, `exitStatusFlags.updateFromStatus`, `updateProgress`) wrapping `Goal::doneSuccess`/`Goal::doneFailure`.

### Type aliases
(none beyond those declared in the header)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-building-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `HookReply` (typedef enum) — values `rpAccept`, `rpDecline`, `rpPostpone`. Purpose: build-hook response.
- `DerivationBuildingGoal` (struct, inherits `Goal`, friend class `Worker`) — purpose: actually build a derivation (no substitution attempts). Public ctor `(const StorePath &, ref<const Derivation>, Worker &, BuildMode, bool storeDerivation)`, public dtor.
  - Private fields: `drvPath` (`const StorePath`), `drv` (`const ref<const Derivation>`), `buildMode` (`const BuildMode`), `mcRunningBuilds` (`std::unique_ptr<MaintainCount<uint64_t>>`).
  - Private nested struct: `LocalBuildCapability` holding `LocalStore & localStore` and `const ExternalBuilder * externalBuilder`.
  - Override `key()` returning `std::string`.
  - Override `jobCategory() const` returning `JobCategory::Build` (defined inline).
  - Private coroutines: `gaveUpOnSubstitution(bool)`, `tryToBuild(StorePathSet)`, `buildWithHook(StorePathSet, std::map<std::string, InitialOutput>, DerivationOptions<StorePath>, PathLocks)`, `buildLocally(LocalBuildCapability, StorePathSet, std::map<std::string, InitialOutput>, DerivationOptions<StorePath>, PathLocks)`.
  - Private members: `tryBuildHook`, `doneFailureLogTooLong`, `queryPartialDerivationOutputMap`, `checkPathValidity`, `doneSuccess`, `doneFailure`, `fixupBuilderFailureErrorMessage`.
  - Forward declarations at file scope: `BuilderFailureError`, `ExternalBuilder`, plus `HookInstance`, `DerivationBuilder` (gated by `#ifndef _WIN32`).

### Functions
(only declarations on the class above)

### Type aliases
- `using std::map;` — namespace-level using-declaration.

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-building-misc.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `PathStatus` (enum struct) — values `Corrupt`, `Absent`, `Valid`.
- `InitialOutputStatus` (struct) — purpose: track an output path with its current `PathStatus`. Fields: `path` (`StorePath`), `status` (`PathStatus`). Inline methods: `isValid()`, `isPresent()`.
- `InitialOutput` (struct) — wraps `std::optional<InitialOutputStatus> known`.

### Functions
- `showKnownOutputs(const StoreDirConfig &, const Derivation &) -> std::string` — declaration, purpose: format known outputs for error messages.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-check.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none — only internal lambdas `getClosure`, `applyChecks`, `checkRefs`)

### Functions
- `checkOutputs(Store &, const StorePath & drvPath, const decltype(Derivation::outputs) & drvOutputs, const decltype(DerivationOptions<StorePath>::outputChecks) & outputChecks, const std::map<std::string, ValidPathInfo> & outputs)` — kind: free function, purpose: enforce per-output `outputChecks` (max size, max closure size, allowed/disallowed references and requisites, fixed-output hash and reference-emptiness). Builds closures via local lambda `getClosure`, applies checks via `applyChecks`/`checkRefs` lambdas; visits both the all-outputs and per-output forms of `outputChecks`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-check.hh

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `checkOutputs(Store &, const StorePath &, const decltype(Derivation::outputs) &, const decltype(DerivationOptions<StorePath>::outputChecks) &, const std::map<std::string, ValidPathInfo> &)` — declaration mirroring derivation-check.cc.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-env-desugar.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `DesugaredEnv::atFileEnvPair(std::string_view name, std::string fileName) -> std::string &` — kind: member function, purpose: register a variable that will point at a sandbox-resident file (`prependBuildDirectory = true`), returning the writable `std::string &` reference into `extraFiles`.
- `DesugaredEnv::create(Store &, const Derivation &, const DerivationOptions<StorePath> &, const StorePathSet &) -> DesugaredEnv` — kind: static factory, purpose: produce the final environment plus extra files; in structured-attrs mode emits `.attrs.sh` / `.attrs.json`, otherwise applies `passAsFile` (synthesising file names like `.attr-<sha256>`) and `exportReferencesGraph`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-env-desugar.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `DesugaredEnv` (struct) — purpose: simplified env-variable + extra-files representation derived from a derivation. Nested struct `EnvEntry` with `prependBuildDirectory` (`bool`, default `false`) and `value` (`std::string`). Fields: `variables` (`std::map<std::string, EnvEntry, std::less<>>`), `extraFiles` (`StringMap`). Members: `atFileEnvPair`, static `create`.
- Forward declarations: `class Store`, `struct Derivation`, `template<typename Input> struct DerivationOptions`.

### Functions
(only the methods of `DesugaredEnv`)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-goal.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `DerivationGoal::DerivationGoal(const StorePath &, ref<const Derivation>, const OutputName &, Worker &, BuildMode, bool storeDerivation)` — kind: ctor, purpose: install `mcExpectedBuilds`, kick off coroutine `haveDerivation(storeDerivation)`.
- `DerivationGoal::key()` — kind: override, purpose: scheduling key (`"db$<name>$<SingleDerivedPath::Built>"`).
- `DerivationGoal::haveDerivation(bool storeDerivation)` — kind: coroutine, purpose: parse drv options, request CA-derivations experimental feature when paths aren't known, attempt substitution of the wanted output via `DrvOutputSubstitutionGoal` and `PathSubstitutionGoal`, schedule a `DerivationResolutionGoal` if needed, then either return success or chain into a resolved `DerivationGoal` or a `DerivationBuildingGoal` (with `preserveFailure = true`).
- `DerivationGoal::repairClosure()` — kind: coroutine, purpose: in `bmRepair` mode, compute the output closure, identify corrupted/missing closure paths, schedule substitution or rebuild via the deriver of those paths, then `doneSuccess(AlreadyValid, ...)`.
- `DerivationGoal::checkPathValidity()` — kind: member function, purpose: query `Realisation` from store and compute `PathStatus` for the wanted output (with CA-derivations realisation backfill).
- `DerivationGoal::assertPathValidity()` — kind: member function, purpose: assert validity returned by `checkPathValidity` and return the realisation.
- `DerivationGoal::doneSuccess(BuildResult::Success::Status, UnkeyedRealisation)` / `DerivationGoal::doneFailure(BuildError)` — kind: member functions, purpose: counter book-keeping (`mcExpectedBuilds.reset()`, `worker.doneBuilds++`/`failedBuilds++`, `exitStatusFlags.updateFromStatus`, `updateProgress`) wrapping `Goal::doneSuccess`/`Goal::doneFailure`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `DerivationGoal` (struct, inherits `Goal`) — purpose: realise a single output of a derivation (purely "administrative" — delegates to other goal types). Public fields: `drvPath` (`StorePath`), `wantedOutput` (`OutputName`). Public ctor (drvPath, drv, wantedOutput, worker, buildMode, storeDerivation) and defaulted dtor. Private fields: `drv` (`ref<const Derivation>`), `buildMode` (`const BuildMode`), `mcExpectedBuilds` (`std::unique_ptr<MaintainCount<uint64_t>>`). Overrides: `key()`, inline `jobCategory() const` returning `JobCategory::Administration`. Private coroutine `haveDerivation(bool)`, `repairClosure()`. Private members `checkPathValidity`, `assertPathValidity`, `doneSuccess`, `doneFailure`.

### Functions
(declarations only)

### Type aliases
- `using std::map;` — namespace-level using-declaration.

### Macros / globals
(none)

## File: src/libstore/build/derivation-resolution-goal.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none — only a function-local `using ValueComparison = decltype(...)`)

### Functions
- `DerivationResolutionGoal::DerivationResolutionGoal(const StorePath &, ref<const Derivation>, Worker &, BuildMode)` — kind: ctor, purpose: install fields, kick off coroutine `resolveDerivation()`.
- `DerivationResolutionGoal::key()` — kind: override, purpose: scheduling key (`"dc$<name>$<printStorePath>"`).
- `DerivationResolutionGoal::resolveDerivation()` — kind: coroutine, purpose: walk `drv->inputDrvs.map`, schedule input goals via a recursive `addWaiteeDerivedPath` lambda (handling dynamic derivations via `childMap`), wait on them, then `tryResolve` against built outputs (or the store DB as a fallback) and produce `resolvedDrv` if needed; emits an `Activity` describing the resolution.

### Type aliases
- `using ValueComparison = decltype([]<typename T>(const ref<T> & lhs, const ref<T> & rhs) { return *lhs < *rhs; })` — local using-declaration, purpose: comparator that compares pointee values for `ref<T>` keys in a goal-tracking map.

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-resolution-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `DerivationResolutionGoal` (struct, inherits `Goal`, friend class `Worker`) — purpose: resolve a derivation by realising its inputs and substituting them in. Public ctor `(const StorePath &, ref<const Derivation>, Worker &, BuildMode)`. Public field `resolvedDrv` (`std::unique_ptr<std::pair<StorePath, BasicDerivation>>`). Private fields: `drvPath` (`StorePath`), `drv` (`ref<const Derivation>`), `buildMode` (`BuildMode`), `act` (`std::unique_ptr<Activity>`). Override `key()`, inline `jobCategory() const` returning `JobCategory::Administration`. Private coroutine `resolveDerivation()`.
- Forward declaration: `struct BuilderFailureError`.

### Functions
(declarations only)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/derivation-trampoline-goal.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `DerivationTrampolineGoal::DerivationTrampolineGoal(ref<const SingleDerivedPath>, const OutputsSpec &, Worker &, BuildMode)` — kind: ctor (induction case), purpose: kick off coroutine `init()` to fetch the drv first, then call `commonInit()`.
- `DerivationTrampolineGoal::DerivationTrampolineGoal(const StorePath &, const OutputsSpec &, const Derivation &, Worker &, BuildMode)` — kind: ctor (base case): kick off coroutine `haveDerivation(drvPath, drv)` directly, then call `commonInit()`.
- `DerivationTrampolineGoal::commonInit()` — kind: member function, purpose: shared ctor logic (sets `name` describing the wanted outputs, calls `worker.updateProgress`).
- `DerivationTrampolineGoal::~DerivationTrampolineGoal()` — explicit empty dtor.
- `DerivationTrampolineGoal::key()` — kind: override, purpose: scheduling key (`"da$<name>$<DerivedPath::Built>"`); internally uses a recursive `pathPartOfReq` lambda walking the `SingleDerivedPath` variant.
- `DerivationTrampolineGoal::init()` — kind: coroutine, purpose: get the drv either via store lookup (resolving the `SingleDerivedPath`, validating it) or by spawning a child goal via `worker.makeGoal(DerivedPath::fromSingle(*drvReq))`, then transition to `haveDerivation`.
- `DerivationTrampolineGoal::haveDerivation(StorePath drvPath, Derivation drv)` — kind: coroutine, purpose: spawn one `DerivationGoal` per wanted output (resolving `OutputsSpec::All` against the drv's outputs), accumulate their built outputs into the trampoline goal's `buildResult`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/derivation-trampoline-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `DerivationTrampolineGoal` (struct, inherits `Goal`) — purpose: outermost goal type; resolves a `SingleDerivedPath` or `StorePath` plus `OutputsSpec` into per-output `DerivationGoal`s. Public fields: `drvReq` (`ref<const SingleDerivedPath>`), `wantedOutputs` (`OutputsSpec`). Two ctors (induction and base case) with `BuildMode buildMode = bmNormal` default. Virtual dtor. Override `key()`, inline `jobCategory() const` returning `JobCategory::Administration`. Private field: `buildMode`. Private coroutines `init()`, `haveDerivation(StorePath, Derivation)`. Private helper `commonInit()`.

### Functions
(declarations only)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/drv-output-substitution-goal.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `DrvOutputSubstitutionGoal::DrvOutputSubstitutionGoal(const DrvOutput & id, Worker & worker)` — kind: ctor, purpose: install `id`, kick off coroutine `init()`.
- `DrvOutputSubstitutionGoal::init()` — kind: coroutine, purpose: query each substituter (via `AsyncCallback<std::shared_ptr<const UnkeyedRealisation>>`) for a realisation; return success on first hit, increment `failedSubstitutions` if any substituter threw, finish with `ecFailed` on substituter failure or `ecNoSubstituters` when none could provide it.
- `DrvOutputSubstitutionGoal::key()` — kind: override, purpose: scheduling key (`"a$<rendered DrvOutput>"`).

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/drv-output-substitution-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `DrvOutputSubstitutionGoal` (class, inherits `Goal`, friend class `Worker`) — purpose: fetch a `Realisation` (drv ⨯ output → output path) from a substituter. Private field `id` (`DrvOutput`). Public ctor `(const DrvOutput &, Worker &)`. Public field `outputInfo` (`std::shared_ptr<const UnkeyedRealisation>`). Public coroutine `init()`. Override `key()`, inline `jobCategory() const` returning `JobCategory::Substitution`.
- Forward declaration: `class Worker`.

### Functions
(declarations only)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/entry-points.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `Store::buildPaths(const std::vector<DerivedPath> &, BuildMode, std::shared_ptr<Store> evalStore)` — kind: out-of-line member, purpose: top-level public API for building one or more derived paths via a `Worker`; collects per-goal failures, throws either the single failure or an aggregate `Error` describing the failed paths.
- `Store::buildPathsWithResults(const std::vector<DerivedPath> &, BuildMode, std::shared_ptr<Store>) -> std::vector<KeyedBuildResult>` — kind: out-of-line member, purpose: build paths and return per-request `KeyedBuildResult`, skipping `ecBusy` goals.
- `Store::buildDerivation(const StorePath &, const BasicDerivation &, BuildMode) -> BuildResult` — kind: out-of-line member, purpose: build a single derivation through a `DerivationTrampolineGoal` and surface the `BuildResult`, packaging exceptions into a `BuildResult::Failure::MiscFailure`.
- `Store::ensurePath(const StorePath &)` — kind: out-of-line member, purpose: substitute the path if not valid; throw on failure with the worker exit status.
- `Store::repairPath(const StorePath &)` — kind: out-of-line member, purpose: attempt substitution with `Repair`; if substitution fails and a valid deriver exists, schedule a rebuild goal via `DerivedPath::Built{...OutputsSpec::All}` in `bmRepair` mode.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/goal-impl.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `Awaiter` (struct, defined inside template `Goal::promise_type::await_transform<T>`) — purpose: bridge an asynchronous callback into the `Goal` coroutine machinery; uses `std::shared_ptr<std::promise<T>>`. Members: `fn` (`fun<void(Callback<T>)>`), `promise` (`std::shared_ptr<std::promise<T>>`), `await_ready()` (always false), `await_suspend(handle_type)` (invokes the callback wrapped in a cross-thread waker enqueue, registers via `Worker::waitForCompletion`), `await_resume()` (returns `promise->get_future().get()`).

### Functions
- `Goal::promise_type::await_transform(AsyncCallback<T> &&) -> Awaiter` — kind: template member, purpose: turn an `AsyncCallback<T>` into an `Awaiter` and register the goal with `Worker::waitForCompletion`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/goal.cc

### Namespaces
- `nix` — definitions for `TimedOut`, `Goal::ChildEvents`, `Goal::Co`, `Goal::promise_type`, `Goal`, plus free functions.

### Classes / structs / enums
(none)

### Functions
- `TimedOut::TimedOut(time_t maxDuration)` — ctor, purpose: pass formatted message to `CloneableError` and store `maxDuration`.
- `Goal::ChildEvents::pushChildEvent(ChildOutput)` — push a child-output event (no-op if timed out).
- `Goal::ChildEvents::pushChildEvent(ChildEOF)` — push EOF event (asserts no prior EOF; no-op if timed out).
- `Goal::ChildEvents::pushChildEvent(TimedOut)` — record timeout, flushing pending output and EOF.
- `Goal::ChildEvents::hasChildEvent() const` — predicate.
- `Goal::ChildEvents::popChildEvent() -> ChildEvent` — pop a single child event in order: outputs, EOF, then timeout.
- `Goal::Co::Co(Co &&) noexcept` / `Goal::Co::operator=(Co &&) noexcept` / `Goal::Co::~Co()` — move-construct, move-assign (destroying any prior handle), and destroy (clearing `alive` and `handle.destroy()`).
- `Goal::promise_type::get_return_object() -> Co` — coroutine helper constructing `Co{handle_type::from_promise(*this)}`.
- `Goal::promise_type::final_awaiter::await_suspend(handle_type) noexcept -> std::coroutine_handle<>` — final-awaiter logic that resumes the continuation if any (replacing `goal->top_co`), else jumps to `std::noop_coroutine()`.
- `Goal::promise_type::return_value(Co && next)` — coroutine return-as-tail-call handler (sets `next` as continuation, threads old continuation onto it).
- `Goal::Co::await_suspend(handle_type) -> std::coroutine_handle<>` — coroutine awaiting another `Co` (sets caller as continuation, becomes `top_co`).
- `CompareGoalPtrs::operator()(const GoalPtr &, const GoalPtr &) const` — compare goals by `keyCached`.
- `addToWeakGoals(WeakGoals &, GoalPtr)` — kind: free function, purpose: add a goal to a `WeakGoals` set if not already present.
- `Goal::await(Goals new_waitees)` — kind: coroutine, purpose: register the goals as waitees (linking back-pointers) and `co_await Suspend{}` until they finish.
- `Goal::doneSuccess(BuildResult::Success)` / `Goal::doneFailure(ExitCode result, BuildResult::Failure failure)` — set `buildResult` and call `amDone(result)`. `doneFailure` asserts `result == ecFailed || ecNoSubstituters`.
- `Goal::amDone(ExitCode result)` — kind: member function, purpose: finalisation: set `exitCode`, log failure (unless `preserveFailure` or no waiters), wake/erase waiters (cancelling siblings if `keepGoing` is off), call `worker.removeGoal`, call `cleanup()`, drop the continuation, return `Done{}`.
- `Goal::trace(std::string_view)` — debug logging helper.
- `Goal::work()` — resume the top coroutine.
- `Goal::handleChildOutput(Descriptor, std::string_view)`, `Goal::handleEOF(Descriptor)`, `Goal::timedOut(TimedOut &&)` — push the corresponding child events and wake the goal.
- `Goal::yield()`, `Goal::waitForAWhile()`, `Goal::waitUntilWoken()`, `Goal::waitForBuildSlot()` — kind: coroutines, purpose: wrappers around `Worker::wakeUp`, `Worker::waitForAWhile`, `Worker::waitForCompletion`, `Worker::waitForBuildSlot` respectively.

### Type aliases
- `Co = nix::Goal::Co`, `promise_type = nix::Goal::promise_type`, `handle_type = nix::Goal::handle_type`, `Suspend = nix::Goal::Suspend` — file-local using-declarations.

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `TimedOut` (struct, final, inherits `CloneableError<TimedOut, BuildError>`) — purpose: error thrown when a build exceeds `maxDuration`. Field: `maxDuration` (`time_t`). Single ctor.
- `CompareGoalPtrs` (struct) — purpose: comparator on `GoalPtr` using `keyCached`. Single member `operator()`.
- `JobCategory` (enum struct) — values `Build`, `Substitution`, `Administration`. Purpose: scheduler hint.
- `Goal` (struct, inherits `std::enable_shared_from_this<Goal>`) — abstract base.
  - Public types: `ChildOutput` (`Descriptor fd`, `std::string data`), `ChildEOF` (`Descriptor fd`).
  - Public type alias `ChildEvent = std::variant<ChildOutput, ChildEOF, std::unique_ptr<TimedOut>>`.
  - Private inner class `ChildEvents` — queue of child events with timeout override; private fields `childOutputs` (`std::queue<ChildOutput>`), `childEOF` (`std::optional<ChildEOF>`), `childTimeout` (`std::unique_ptr<TimedOut>`); public methods `pushChildEvent` (three overloads), `hasChildEvent` (const), `popChildEvent`.
  - Private fields: `waitees` (`Goals`), `cachedKey` (`std::optional<std::string>`), `childEvents`.
  - Public enum: `ExitCode` (typedef enum: `ecBusy`, `ecSuccess`, `ecFailed`, `ecNoSubstituters`).
  - Public fields: `worker` (`Worker &`), `waiters` (`WeakGoals`), `nrFailed` (default 0), `nrNoSubstituters` (default 0), `name`, `exitCode` (default `ecBusy`), `buildResult` (`BuildResult`).
  - Inner tag types: `Suspend` (empty), `Return` (empty), `Done` (empty, `[[nodiscard]]`, private ctor; friend `Goal`), `WaitForChildEvent` (empty).
  - `Co` (struct, `[[nodiscard]]`) — coroutine wrapper with `handle` (`handle_type`), explicit ctor, move ops, deleted copy, dtor; awaiter members `await_ready`, `await_suspend`, `await_resume`.
  - Template `AsyncCallback<T>` (struct) — wraps `fun<void(Callback<T>)> fn`.
  - `InitialSuspend` (struct) — initial-suspend awaiter that records the handle and asserts goal/top_co invariants on `await_resume`.
  - `promise_type` (struct) — fields `continuation` (`std::optional<Co>`), `goal` (`Goal *`, default `nullptr`), `alive` (`bool`, default `true`); inner struct `final_awaiter` with `await_ready`, `await_suspend`, `await_resume`. Members `get_return_object`, inline `initial_suspend()` returning empty `InitialSuspend`, inline `final_suspend() noexcept` returning empty `final_awaiter`, `return_value(Return)` (no-op), `return_value(Done)` (no-op), `return_value(Co &&)`, `unhandled_exception` (rethrows), `await_transform(Co &&)`, template `await_transform(AsyncCallback<T> &&)` (defined in goal-impl.hh). Inner awaiter `SuspendAwaiter` (asserts no pending child events) used by `await_transform(Suspend)`. Inner awaiter `ChildEventAwaiter` used by `await_transform(WaitForChildEvent)`.
  - Public type alias `handle_type = std::coroutine_handle<promise_type>`.
  - Protected field: `top_co` (`std::optional<Co>`).
  - Protected methods: `Done amDone(ExitCode)`, `Done doneSuccess(BuildResult::Success)`, `Done doneFailure(ExitCode, BuildResult::Failure)`, `Co await(Goals)`, `Co waitForAWhile()`, `Co waitUntilWoken()`, `Co waitForBuildSlot()`, `Co yield()`.
  - Public methods: virtual `cleanup()` (default no-op), public field `preserveFailure` (default false), ctor `(Worker &, Co init)` (sets `top_co.handle.promise().goal = this`), virtual dtor (logs `goal destroyed`), `work()`, `handleChildOutput`, `handleEOF`, `timedOut`, `trace`, inline `getName() const`, pure virtual `key()`, inline `keyCached() &` (memoising), pure virtual `jobCategory() const`.
- Forward declarations: `struct Goal`, `class Worker`.

### Functions
- `addToWeakGoals(WeakGoals &, GoalPtr)` — declaration.

### Type aliases
- `GoalPtr = std::shared_ptr<Goal>` — typedef.
- `WeakGoalPtr = std::weak_ptr<Goal>` — typedef.
- `Goals = std::set<GoalPtr, CompareGoalPtrs>` — typedef.
- `WeakGoals = std::set<WeakGoalPtr, std::owner_less<WeakGoalPtr>>` — typedef.
- `WeakGoalMap = std::map<StorePath, WeakGoalPtr>` — typedef.
- `Goal::handle_type = std::coroutine_handle<promise_type>` — using.

### Macros / globals
- `template<typename... ArgTypes> struct std::coroutine_traits<nix::Goal::Co, ArgTypes...>` — specialisation with `using promise_type = nix::Goal::promise_type;`, purpose: tell the standard library how to find the promise type for a `Goal::Co` coroutine.

## File: src/libstore/build/substitution-goal.cc

### Namespaces
- `nix` — definitions for `PathSubstitutionGoal`.

### Classes / structs / enums
(none)

### Functions
- `PathSubstitutionGoal::PathSubstitutionGoal(const StorePath &, Worker &, RepairFlag, std::optional<ContentAddress>)` — kind: ctor, purpose: install fields, kick off coroutine `init()`, set up `maintainExpectedSubstitutions`.
- `PathSubstitutionGoal::~PathSubstitutionGoal()` — kind: dtor, purpose: call `cleanup()`.
- `PathSubstitutionGoal::init()` — kind: coroutine, purpose: register temp root, iterate substituters via `worker.getSubstituters()`, query path info (`AsyncCallback<ref<const ValidPathInfo>>`), reconstruct path on differing storeDir for CA paths, enforce trust of substitute signatures, schedule recursive substitution of references, call `tryToRun`. On failure, finishes via `doneFailure(ecNoSubstituters | ecFailed, ...)`.
- `PathSubstitutionGoal::tryToRun(StorePath subPath, ref<Store> sub, std::shared_ptr<const ValidPathInfo> info, bool & substituterFailed)` — kind: coroutine, purpose: wait for a substitution slot, run `copyStorePath` in a worker thread (with `Activity` and `PushActivity`), tally counters (`maintainExpected*`, `worker.doneSubstitutions++`, `doneDownloadSize`, `doneNarSize`), finalise success (`Substituted`) or failure.
- `PathSubstitutionGoal::cleanup()` — kind: final override, purpose: join the worker thread on destruction, calling `worker.childTerminated(this, JobCategory::Substitution)` afterwards.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/substitution-goal.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `PathSubstitutionGoal` (struct, inherits `Goal`) — purpose: substitute one store path. Public fields: `storePath` (`StorePath`), `repair` (`RepairFlag`), `thr` (`std::thread`), four `std::unique_ptr<MaintainCount<uint64_t>>` (`maintainExpectedSubstitutions`, `maintainRunningSubstitutions`, `maintainExpectedNar`, `maintainExpectedDownload`), `ca` (`std::optional<ContentAddress>`). Public ctor `(const StorePath &, Worker &, RepairFlag = NoRepair, std::optional<ContentAddress> = std::nullopt)`. Public dtor. Inline `key()` override (`"a$<name>$<printStorePath>"`). Public coroutines `init()`, `gotInfo()` (declared but not defined), `tryToRun(StorePath, ref<Store>, std::shared_ptr<const ValidPathInfo>, bool &)`, `finished()` (declared but not defined). Override `cleanup()` final. Inline `jobCategory() const` returning `JobCategory::Substitution`.

### Functions
(declarations only)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/build/worker.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `Worker::Worker(Store & store, Store & evalStore)` — kind: ctor, purpose: build the cross-thread `Waker` (via `ref` and private constructor), default-create activities (`act` `actRealise`, `actDerivations` `actBuilds`, `actSubstitutions` `actCopyPaths`), create IO completion port on Windows, take `getSubstituters` lambda from `nix::settings`, initialise counters.
- `Worker::~Worker()` — kind: dtor, purpose: drop top goals; assert `expectedSubstitutions`, `expectedDownloadSize`, `expectedNarSize` are all zero.
- `Worker::initGoalIfNeeded<G>(std::weak_ptr<G> &, Args &&...)` — kind: private template, purpose: look up an existing goal in the slot or construct a new one (and `wakeUp` it).
- `Worker::makeDerivationTrampolineGoal(ref<const SingleDerivedPath>, const OutputsSpec &, BuildMode)` — kind: private factory using `derivationTrampolineGoals.ensureSlot(*drvReq).value[wantedOutputs]`.
- `Worker::makeDerivationTrampolineGoal(const StorePath &, const OutputsSpec &, const Derivation &, BuildMode)` — kind: public factory.
- `Worker::makeDerivationGoal(const StorePath &, ref<const Derivation>, const OutputName &, BuildMode, bool storeDerivation)` — purpose: cached factory keyed by drvPath × outputName.
- `Worker::makeDerivationResolutionGoal(const StorePath &, ref<const Derivation>, BuildMode)` — purpose: cached factory keyed by drvPath.
- `Worker::makeDerivationBuildingGoal(const StorePath &, ref<const Derivation>, BuildMode, bool storeDerivation)` — purpose: cached factory keyed by drvPath.
- `Worker::makePathSubstitutionGoal(const StorePath &, RepairFlag, std::optional<ContentAddress>)` — purpose: cached factory.
- `Worker::makeDrvOutputSubstitutionGoal(const DrvOutput &)` — purpose: cached factory.
- `Worker::makeGoal(const DerivedPath &, BuildMode)` — purpose: dispatch on `DerivedPath::Built` vs `DerivedPath::Opaque` to the trampoline factory or path substitution factory.
- `Worker::removeGoal(GoalPtr)` — purpose: remove the goal from the per-kind map (one branch per concrete subtype: `DerivationTrampolineGoal`, `DerivationGoal`, `DerivationResolutionGoal`, `DerivationBuildingGoal`, `PathSubstitutionGoal`, `DrvOutputSubstitutionGoal`) and from `topGoals`; cancel siblings if a top goal failed and `keepGoing` is off.
- `Worker::wakeUp(GoalPtr)` — purpose: trace and add to `awake` weak-goal set.
- `Worker::getNrLocalBuilds()`, `Worker::getNrSubstitutions()` — accessors.
- `Worker::childStarted(GoalPtr, const std::set<MuxablePipePollState::CommChannel> &, bool inBuildSlot, bool respectTimeouts)` — purpose: register a running child process and bump the relevant slot counter (`nrSubstitutions` or `nrLocalBuilds`); `Administration` is `unreachable`.
- `Worker::childTerminated(Goal *)` — overload calling `goal->jobCategory()` then forwarding to the second overload.
- `Worker::childTerminated(Goal *, JobCategory)` — purpose: locate the child by raw pointer, decrement the right slot counter, erase, then wake at most one waiter on the matching `wantingToBuild`/`wantingToSubstitute` set.
- `Worker::waitForBuildSlot(GoalPtr)` — purpose: wake the goal immediately if a slot is free, else add it to the appropriate waiting set.
- `Worker::waitForAWhile(GoalPtr)` — purpose: add to `waitingForAWhile`.
- `Worker::waitForCompletion(GoalPtr)` — purpose: add to `waitingForCompletion`.
- `Worker::run(const Goals & topGoals)` — purpose: main loop: drive `awake` goals, call `waitForInput`, handle `keepGoing`/`maxBuildJobs == 0` edge cases (throwing if no progress is possible), assert wind-down invariants when `keepGoing` is off.
- `Worker::waitForInput()` — purpose: poll child fds and the cross-thread waker pipe (or completion port), dispatch output/EOF/timeouts, periodically wake `waitingForAWhile` goals.
- `Worker::getCrossThreadWaker()` — return weak pointer to the `Waker`.
- `Worker::Waker::wakeAll(Worker &)` — purpose: drain the cross-thread queue (under `Sync` lock), drain the wakeup pipe on POSIX, and wake the corresponding goals (erasing them from `waitingForCompletion`).
- `Worker::Waker::enqueue(WeakGoalPtr)` — purpose: enqueue a wakeup via the pipe (POSIX) or completion port (Windows).
- `Worker::pathContentsGood(const StorePath &)` — purpose: cached check that on-disk path matches `narHash`.
- `Worker::markContentsGood(const StorePath &)` — purpose: cache override.
- `upcast_goal(std::shared_ptr<PathSubstitutionGoal>)`, `upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal>)`, `upcast_goal(std::shared_ptr<DerivationGoal>)` — kind: free functions, purpose: convert a concrete goal pointer to a `GoalPtr` from a place that doesn't see the concrete type.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/include/nix/store/build/worker.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `Child` (struct) — purpose: track a running child process and its comm channels. Fields: `goal` (`WeakGoalPtr`), `goal2` (`Goal *`, "ugly hackery"), `channels` (`std::set<MuxablePipePollState::CommChannel>`), `respectTimeouts` (`bool`), `inBuildSlot` (`bool`), `lastOutput` (`steady_time_point`), `timeStarted` (`steady_time_point`).
- `Worker` (class) — coordinator of all goals. Inner private class `Waker` with cross-thread queue + wakeup pipe (POSIX) or completion port (Windows); `friend class Worker` inside `Waker`.
  - Private fields: `topGoals` (`Goals`), `awake`, `wantingToBuild`, `wantingToSubstitute` (all `WeakGoals`), `children` (`std::list<Child>`), `nrLocalBuilds`, `nrSubstitutions` (both `size_t`), goal caching maps: `derivationTrampolineGoals` (`DerivedPathMap<std::map<OutputsSpec, std::weak_ptr<DerivationTrampolineGoal>>>`), `derivationGoals` (`std::map<StorePath, std::map<OutputName, std::weak_ptr<DerivationGoal>>>`), `derivationResolutionGoals` (`std::map<StorePath, std::weak_ptr<DerivationResolutionGoal>>`), `derivationBuildingGoals` (`std::map<StorePath, std::weak_ptr<DerivationBuildingGoal>>`), `substitutionGoals` (`std::map<StorePath, std::weak_ptr<PathSubstitutionGoal>>`), `drvOutputSubstitutionGoals` (`std::map<DrvOutput, std::weak_ptr<DrvOutputSubstitutionGoal>>`), `waitingForAWhile`, `waitingForCompletion`, `lastWokenUp` (`steady_time_point`), `pathContentsGoodCache` (`std::map<StorePath, bool>`), `wakerState` (`ref<Waker>`).
  - Public fields: `act`, `actDerivations`, `actSubstitutions` (all `const Activity`), `exitStatusFlags` (`ExitStatusFlags`), `ioport` (`AutoCloseFD`, Windows), `store`, `evalStore` (both `Store &`), `settings` (`const WorkerSettings &`), `getSubstituters` (`fun<std::list<ref<Store>>()>`), `hook` (`std::unique_ptr<HookInstance>`, POSIX), counters `expectedBuilds`, `doneBuilds`, `failedBuilds`, `runningBuilds`, `expectedSubstitutions`, `doneSubstitutions`, `failedSubstitutions`, `runningSubstitutions`, `expectedDownloadSize`, `doneDownloadSize`, `expectedNarSize`, `doneNarSize` (all `uint64_t`), `tryBuildHook` (`bool`, default true).
  - Public methods: ctor `(Store &, Store &)`, dtor, all public `make*Goal` factories, `removeGoal`, `wakeUp`, `getCrossThreadWaker`, `getNrLocalBuilds`, `getNrSubstitutions`, `childStarted`, `childTerminated` (two overloads), `waitForBuildSlot`, `waitForAWhile`, `waitForCompletion`, `run`, `waitForInput`, `pathContentsGood`, `markContentsGood`, inline `updateProgress`.
- Forward declarations: `struct WorkerSettings`, `struct DerivationTrampolineGoal`, `struct DerivationGoal`, `struct DerivationResolutionGoal`, `struct DerivationBuildingGoal`, `struct PathSubstitutionGoal`, `class DrvOutputSubstitutionGoal`, plus `struct HookInstance` under `#ifndef _WIN32`.

### Functions
- `upcast_goal(std::shared_ptr<PathSubstitutionGoal>)`, `upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal>)`, `upcast_goal(std::shared_ptr<DerivationGoal>)` — three free-function declarations.

### Type aliases
- `steady_time_point = std::chrono::time_point<std::chrono::steady_clock>` — typedef.

### Macros / globals
(none)

## File: src/libstore/builtins/buildenv.cc

### Namespaces
- `nix` (with anonymous namespace inside).

### Classes / structs / enums
- `State` (struct, anonymous-namespace) — purpose: track current symlink priorities and counter while building a profile. Fields: `priorities` (`std::map<std::filesystem::path, int>`), `symlinks` (`unsigned long`, default 0).

### Functions
- `RegisterBuiltinBuilder::builtinBuilders() -> RegisterBuiltinBuilder::BuiltinBuilders &` — kind: out-of-line static method, purpose: return the function-local static registry of builtin builders.
- `static void createLinks(State &, const std::filesystem::path & srcDir, const std::filesystem::path & dstDir, int priority)` — kind: file-local static, purpose: recursive symlink-creation routine that resolves directory collisions by canonicalising and recursing, special-cases `propagated-build-inputs`, `nix-support`, `perllocal.pod`, `info/dir`, `log`, `manifest.nix`, `manifest.json`. Throws `BuildEnvFileConflictError` on equal-priority collisions.
- `buildProfile(const std::filesystem::path & out, Packages && pkgs)` — kind: free function, purpose: build a Nix profile from a list of `Package`s, processing in priority order, then handling `propagated-user-env-packages` with reduced priority (counter starting at 1000) using a worklist.
- `static void builtinBuildenv(const BuiltinBuilderContext & ctx)` — kind: file-local static, purpose: parse the `derivations` env-var (active/priority/outputCount blocks), call `buildProfile`, link the manifest.

### Type aliases
(none)

### Macros / globals
- `static RegisterBuiltinBuilder registerBuildenv("buildenv", builtinBuildenv);` — registers `buildenv` in the global builtin-builder map.

## File: src/libstore/include/nix/store/builtins/buildenv.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `Package` (struct) — purpose: minimal "store-level package attrset" used by buildenv. Fields: `path` (`std::filesystem::path`), `active` (`bool`), `priority` (`int`). Single explicit ctor.
- `BuildEnvFileConflictError` (class, final, inherits `CloneableError<BuildEnvFileConflictError, Error>`) — purpose: error thrown when two paths claim the same target with the same priority. Public fields: `fileA` (`const std::filesystem::path`), `fileB` (`const std::filesystem::path`), `priority` (`int`). Single ctor.

### Functions
- `buildProfile(const std::filesystem::path & out, Packages && pkgs)` — declaration.

### Type aliases
- `Packages = std::vector<Package>` — typedef.

### Macros / globals
(none)

## File: src/libstore/include/nix/store/builtins.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `BuiltinBuilderContext` (struct) — purpose: arguments passed to a builtin builder. Fields: `drv` (`const BasicDerivation &`), `outputs` (`std::map<std::string, std::string>`), `netrcData` (`std::string`), `caFileData` (`std::string`), `hashedMirrors` (`Strings`), `tmpDirInSandbox` (`std::filesystem::path`), `awsCredentials` (`std::optional<AwsCredentials>`, gated on `NIX_WITH_AWS_AUTH`).
- `RegisterBuiltinBuilder` (struct) — purpose: register a builtin builder by name. Nested typedef `BuiltinBuilders = std::map<std::string, BuiltinBuilder>`. Static accessor `builtinBuilders()`. Inline ctor `(const std::string & name, BuiltinBuilder && builder)` inserts into the registry.

### Functions
(only the `RegisterBuiltinBuilder` ctor and `builtinBuilders()` accessor)

### Type aliases
- `BuiltinBuilder = fun<void(const BuiltinBuilderContext &)>` — using.
- `RegisterBuiltinBuilder::BuiltinBuilders = std::map<std::string, BuiltinBuilder>` — typedef inside the struct.

### Macros / globals
(none)

## File: src/libstore/builtins/fetchurl.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `static void builtinFetchurl(const BuiltinBuilderContext & ctx)` — kind: file-local static, purpose: implement `builtin:fetchurl`: write netrc / caFile to sandbox tmp dir, validate that the drv is fixed-output or impure, fetch from hashed mirrors first (only when CA method is `Flat`) then `mainUrl`, optionally unpack `.xz`, optionally `chmod 0755` if `executable=1`. Uses pre-resolved AWS credentials (when `NIX_WITH_AWS_AUTH`) for `s3://` URLs.

### Type aliases
(none)

### Macros / globals
- `static RegisterBuiltinBuilder registerFetchurl("fetchurl", builtinFetchurl);`

## File: src/libstore/builtins/unpack-channel.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `static void builtinUnpackChannel(const BuiltinBuilderContext & ctx)` — kind: file-local static, purpose: implement `builtin:unpack-channel`: validate `channelName` has no separators, untar `src` to `out`, ensure exactly one top-level entry, rename it to `channelName`.

### Type aliases
(none)

### Macros / globals
- `static RegisterBuiltinBuilder registerUnpackChannel("unpack-channel", builtinUnpackChannel);`

## File: src/libstore/unix/build/child.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `commonChildInit()` — kind: free function, purpose: shared child-process setup: install simple logger, `restoreProcessContext(false)`, `setsid` (separate session/process group), dup `STDERR_FILENO` to `STDOUT_FILENO`, route `STDIN_FILENO` from `/dev/null` (opened `O_RDWR | O_CLOEXEC`).

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/include/nix/store/build/child.hh

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `commonChildInit()` — declaration.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/chroot-derivation-builder.cc

### Namespaces
- `nix` (file is included into `derivation-builder.cc` and entire body is gated on `#if defined(__linux__) || defined(__FreeBSD__)`).

### Classes / structs / enums
- `ChrootDerivationBuilder` (struct, virtual inherits `DerivationBuilderImpl`) — purpose: shared chroot-based builder behaviour for Linux and FreeBSD. Public fields: `chrootRootDir` (`std::filesystem::path`), `autoDelChroot` (`std::optional<AutoDelete>`), `pathsInChroot` (`PathsInChroot`). Ctor delegates to `DerivationBuilderImpl`.
  - Override `needsHashRewrite()` returns `false`.
  - Override `setBuildTmpDir()` — sets `tmpDir = topTmpDir / "build"` with mode 0700.
  - Override `tmpDirInSandbox()` — returns the configured `sandboxBuildDir`.
  - Virtual `sandboxGid()` returns `buildUser->getGID()`.
  - Override `prepareSandbox()` — set up `BuildChrootParams`, call `setupBuildChroot`, populate `pathsInChroot` from input closure (`store.toRealPath(i)` for each input path), prune any expected outputs already in the sandbox.
  - Override `getPreBuildHookArgs()` — returns `Strings({drvPath, chrootRootDir})`.
  - Override `realPathInHost(const std::filesystem::path &)` — chroot-aware mapping (if `!needsHashRewrite()`, prepends `chrootRootDir`).
  - Override `cleanupBuild(bool force)` — call base, then move outputs back from chroot to their real paths (when not `bmCheck`), then drop `autoDelChroot`.
  - `addDependencyPrep(const StorePath &) -> std::pair<std::filesystem::path, std::filesystem::path>` — helper returning `(source, target)` for materialising a store path inside the chroot; throws if target already exists.

### Functions
(only members above)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/chroot.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `setupBuildChroot(const BuildChrootParams &) -> std::pair<std::filesystem::path, AutoDelete>` — kind: free function, purpose: create the chroot directory hierarchy (`<chrootParentDir>/root` plus `/tmp` 01777, `/etc`, store dir 01775), apply ownership (chown to build user when `useUidRange` & UID count ≠ 1), set up `/etc/hosts` with localhost entries when `isSandboxed`, return the root path and an `AutoDelete` for cleanup. Throws if `useUidRange` is set without sufficient build-user UID range (referencing `autoAllocateUids` setting name).

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/chroot.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `BuildChrootParams` (struct) — purpose: parameter pack for `setupBuildChroot`. Fields: `chrootParentDir` (`std::filesystem::path`), `useUidRange` (`bool`), `isSandboxed` (`bool`), `buildUser` (`UserLock *`), `storeDir` (`std::string`), `chownToBuilder` (`std::function<void(const std::filesystem::path &)>`).
- Forward declarations: `class AutoDelete`, `struct UserLock`.

### Functions
- `setupBuildChroot(const BuildChrootParams &) -> std::pair<std::filesystem::path, AutoDelete>` — declaration.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/darwin-derivation-builder.cc

### Namespaces
- `nix` (file gated by `#ifdef __APPLE__`; the `IpcsCommand` struct is at file scope, before `nix`).

### Classes / structs / enums
- `IpcsCommand` (struct, file-scope, before `nix` namespace) — purpose: kernel sysctl command struct for iterating Darwin SysV IPC objects. Fields: `ipcs_magic` (`uint32_t`), `ipcs_op` (`uint32_t`), `ipcs_cursor` (`uint32_t`), `ipcs_datalen` (`uint32_t`), `ipcs_data` (`void *`).
- `DarwinDerivationBuilder` (struct, inherits `DerivationBuilderImpl`) — purpose: macOS-specific builder using `sandbox_init_with_parameters`. Fields: `pathsInChroot` (`PathsInChroot`), `useSandbox` (`bool`). Ctor stores `useSandbox`.
  - Override `prepareSandbox()` — populate `pathsInChroot` from `getPathsInSandbox`.
  - Override `setUser()` — call base, then build the sandbox profile (header `(version 1)`, deny-default with optional logging, sandbox-defaults.sb, optional sandbox-network.sb for non-sandboxed FODs, allow lists for output paths split into multiple groups to avoid the macOS sandbox interpreter limit, allow lists for input paths covering `(subpath …)` for directories and `(literal …)` for files, allow file-read on ancestor directories, optional `additionalSandboxProfile`); when `_NIX_TEST_NO_SANDBOX != 1` call `sandbox_init_with_parameters` with `_NIX_BUILD_TOP`, `_GLOBAL_TMP_DIR`, optional `_ALLOW_LOCAL_NETWORKING`.
  - Override `execBuilder(const Strings &, const Strings &)` — initialise `posix_spawnattr_t` with `POSIX_SPAWN_SETEXEC`, on `aarch64-darwin` clear `kern.curproc_arch_affinity` and pin to `CPU_TYPE_ARM64` (escape Rosetta), on `x86_64-darwin` pin to `CPU_TYPE_X86_64`, then `posix_spawn`.
  - `cleanupSysVIPCForUser(uid_t)` — iterate shared memory, message queues, and semaphores via `sysctlbyname` using the `IpcsCommand` struct and remove (`shmctl`/`msgctl`/`semctl` with `IPC_RMID`) those owned by the build uid.
  - Override `killSandbox(bool getStats)` — call `DerivationBuilderImpl::killSandbox(getStats)`, then `cleanupSysVIPCForUser(buildUser->getUID())`.

### Functions
- `extern "C" int sandbox_init_with_parameters(const char *, uint64_t, const char * const [], char **)` — forward declaration of undocumented Darwin sandbox API.

### Type aliases
(none)

### Macros / globals
- `IPCS_MAGIC` (0x00000001), `IPCS_SHM_ITER` (0x00000002), `IPCS_SEM_ITER` (0x00000020), `IPCS_MSG_ITER` (0x00000200), `IPCS_SHM_SYSCTL` (`"kern.sysv.ipcs.shm"`), `IPCS_MSG_SYSCTL` (`"kern.sysv.ipcs.msg"`), `IPCS_SEM_SYSCTL` (`"kern.sysv.ipcs.sem"`) — IPC sysctl constants.

## File: src/libstore/unix/build/derivation-builder.cc

### Namespaces
- `nix` (re-opened twice; second reopening is after the platform-specific `#include`s to define the deleter and the factory).

### Classes / structs / enums
- `NotDeterministic` (struct, final, inherits `CloneableError<NotDeterministic, BuildError>`) — purpose: thrown when a check-mode build differs from the prior result. Variadic ctor (forwards args to `CloneableError(BuildResult::Failure::NotDeterministic, ...)`); sets `isNonDeterministic = true`.
- `DerivationBuilderImpl` (class, inherits `DerivationBuilder` and `DerivationBuilderParams`) — the concrete unix builder. // FIXME comment indicates rename to `UnixDerivationBuilder`.
  - Protected fields: `pid` (`Pid`), `store` (`LocalStore &`), `localSettings` (`const LocalSettings &`, default-initialised from `store.config->getLocalSettings()`), `miscMethods` (`std::unique_ptr<DerivationBuilderCallbacks>`), `buildUser` (`std::unique_ptr<UserLock>`), `tmpDir` and `topTmpDir` (`std::filesystem::path`), `tmpDirFd` (`AutoCloseFD`), `derivationType` (`const DerivationType`, cached from `drv.type()`), `env` (`Environment` typedef of `StringMap`), `inputRewrites`, `outputRewrites` (both `StringMap`), `redirectedOutputs` (`RedirectedOutputs` typedef of `std::map<StorePath, StorePath>`), `scratchOutputs` (`OutputPathMap`), `daemonSocket` (`AutoCloseFD`), `daemonThread` (`std::thread`), `daemonWorkerThreads` (`std::list<DaemonWorkerState>`).
  - Public ctor `(LocalStore &, std::unique_ptr<DerivationBuilderCallbacks>, DerivationBuilderParams)`; public `cleanupOnDestruction() noexcept` (invoked from the deleter; calls `killChild`/`stopDaemon`/`cleanupBuild(false)` swallowing exceptions).
  - Override `originalPaths() -> const StorePathSet &` (returns `inputPaths`).
  - Override `isAllowed(const StorePath &)` (consults `inputPaths` and `state_->addedPaths` from inherited `RestrictionContext`).
  - Override `isAllowed(const DrvOutput &)`.
  - Plus non-virtual `isAllowed(const DerivedPath &)` (declared but defined via the same name).
  - Friend struct `RestrictedStore`.
  - Virtual `needsHashRewrite()` (default `true`).
  - Public overrides: `startBuild() -> std::optional<Descriptor>`, `unprepareBuild() -> SingleDrvOutputs`, `killChild() -> bool`.
  - Virtual `getBuildUser()` (default unprivileged user lock via `acquireUserLock(..., 1, false)`), `setBuildTmpDir`, `tmpDirInSandbox`, `prepareUser` (calls `killSandbox(false)`), `prepareSandbox` (errors on `useUidRange`), `getPreBuildHookArgs`, `realPathInHost`, `startChild`, `enterChroot` (no-op), `setUser`, `execBuilder` (default `execve`), `cleanupBuild`, `killSandbox`.
  - Helper methods: `openSlave`, `processSandboxSetupMessages`, `initEnv`, `startDaemon`, `stopDaemon`, `addDependencyImpl(const StorePath &)` (default empty), `chownToBuilder` (path overload and fd overload), `writeBuilderFile`, `runChild(RunChildArgs)`, `registerOutputs`, `decideWhetherDiskFull`, `makeFallbackPath(const StorePath &)`, `makeFallbackPath(OutputNameView)`, plus `preResolveAwsCredentials()` under `NIX_WITH_AWS_AUTH`.
  - Nested types: typedef `Environment = StringMap`; typedef `RedirectedOutputs = std::map<StorePath, StorePath>`; struct `DaemonWorkerState` (`thread`, `done` (`ref<std::atomic_flag>`)); struct `RunChildArgs` (`std::optional<AwsCredentials> awsCredentials` under `NIX_WITH_AWS_AUTH`).
  - Local types inside `registerOutputs`: `AlreadyRegistered` (struct with `path`), `PerhapsNeedToRegister` (struct with `refs` and `otherOutputs`).
  - Anonymous local enum inside `getPathsInSandbox`: `BuildHookState { stBegin, stExtraChrootDirs }`.

### Functions
- `preserveDeathSignal(fun<void()> setCredentials)` — kind: free function, purpose: Linux-only `PR_SET_PDEATHSIG` preservation across credential changes (uses `getppid` race check); no-op elsewhere.
- `static void handleDiffHook(const std::filesystem::path & diffHook, uid_t, uid_t, const std::filesystem::path & tryA, const std::filesystem::path & tryB, const std::filesystem::path & drvPath, const std::filesystem::path & tmpDir)` — kind: file-local static, purpose: invoke the configured `diff-hook` program when a check-mode comparison fails; logs errors via `logError`.
- `static void movePath(const std::filesystem::path &, const std::filesystem::path &)` — kind: file-local static, purpose: rename a path with temporary owner-write permission tweaks for non-root processes.
- `static void replaceValidPath(const std::filesystem::path & storePath, const std::filesystem::path & tmpPath)` — kind: file-local static, purpose: atomic-ish replacement of a store path, keeping a backup until the new path is in place; recovers the backup if the move throws.
- `static void rethrowExceptionAsError()` — kind: file-local static, purpose: rethrow the current exception wrapped as `Error` (preserves `Error` subclasses).
- `static void handleChildException(bool sendException)` — kind: file-local static, purpose: serialise the in-flight exception over `STDERR_FILENO` (`\1` framing) so the parent can read it via `processSandboxSetupMessages`; otherwise prints the message.
- `static void checkNotWorldWritable(std::filesystem::path)` — kind: file-local static, purpose: walk parent directories to ensure none are world-writable or symlinks.
- `DerivationBuilderImpl::killSandbox`, `killChild`, `unprepareBuild`, `decideWhetherDiskFull`, `startBuild`, `getPathsInSandbox`, `prepareSandbox`, `openSlave`, `preResolveAwsCredentials` (NIX_WITH_AWS_AUTH), `startChild`, `processSandboxSetupMessages`, `initEnv`, `startDaemon`, `stopDaemon`, `addDependencyImpl`, `chownToBuilder` (path/fd), `writeBuilderFile`, `runChild`, `setUser`, `execBuilder`, `registerOutputs`, `cleanupBuild`, `makeFallbackPath` (two overloads) — see corresponding members and overrides above.
- `DerivationBuilderDeleter::operator()(DerivationBuilder *) noexcept` — call `cleanupOnDestruction` if it `dynamic_cast`s to `DerivationBuilderImpl`, then `delete`.
- `makeDerivationBuilder(LocalStore &, std::unique_ptr<DerivationBuilderCallbacks>, DerivationBuilderParams)` — factory: applies sandbox-mode policy (`smEnabled`/`smDisabled`/`smRelaxed`), forces `useSandbox` for diverted store on Linux/FreeBSD, auto-disables sandboxing on Linux without namespace support if `sandboxFallback`, errors on `uid-range` without sandbox; selects `DarwinDerivationBuilder` (macOS), `ChrootLinuxDerivationBuilder` or `LinuxDerivationBuilder` (Linux), `ChrootFreeBSDDerivationBuilder` or `FreeBSDDerivationBuilder` (FreeBSD), or `DerivationBuilderImpl` (other) — errors out if sandboxing requested on unsupported platform.
- `makeExternalDerivationBuilder(...)` — factory defined in `external-derivation-builder.cc` (included into this TU).

### Type aliases
- `Environment = StringMap` (member typedef on `DerivationBuilderImpl`).
- `RedirectedOutputs = std::map<StorePath, StorePath>` (member typedef).

### Macros / globals
- `const std::filesystem::path DerivationBuilderImpl::homeDir = "/homeless-shelter";` — sentinel HOME path passed to builders.
- `#include "chroot-derivation-builder.cc"`, `#include "linux-derivation-builder.cc"`, `#include "freebsd-derivation-builder.cc"`, `#include "darwin-derivation-builder.cc"`, `#include "external-derivation-builder.cc"` — pulls platform variants into this translation unit so they all see `DerivationBuilderImpl`.

## File: src/libstore/unix/build/external-derivation-builder.cc

### Namespaces
- `nix` (included into `derivation-builder.cc`).

### Classes / structs / enums
- `ExternalDerivationBuilder` (struct, inherits `DerivationBuilderImpl`) — purpose: hand the build over to an external program described by `ExternalBuilder`. Field: `externalBuilder` (`ExternalBuilder`). Ctor requires `Xp::ExternalBuilders` experimental feature.
  - Override `tmpDirInSandbox()` returns `"/build"`.
  - Override `setBuildTmpDir()` sets `tmpDir = topTmpDir / "build"` with mode 0700.
  - Override `startChild()` — error out for `recursive-nix`, build a JSON object describing builder, args, env, paths, outputs (writing it to `<topTmpDir>/build.json`), then `startProcess` exec-ing the external program (with `commonChildInit`, `chdir(tmpDir)`, `chownToBuilder(topTmpDir)`, `setUser`).

### Functions
- `makeExternalDerivationBuilder(LocalStore &, std::unique_ptr<DerivationBuilderCallbacks>, DerivationBuilderParams, const ExternalBuilder &) -> DerivationBuilderUnique` — factory returning an `ExternalDerivationBuilder` via `DerivationBuilderUnique`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/freebsd-derivation-builder.cc

### Namespaces
- `nix` (with anonymous namespace inside; included into `derivation-builder.cc`; gated by `#ifdef __FreeBSD__`).

### Classes / structs / enums
- `PasswordEntry` (struct, anonymous-namespace) — purpose: in-memory record matching FreeBSD passwd db row. Fields: `name` (`std::string`), `uid` (`uid_t`), `gid` (`gid_t`), `description` (`std::string`), `home` (`std::filesystem::path`), `shell` (`std::filesystem::path`).
- `FreeBSDDerivationBuilder` (struct, virtual inherits `DerivationBuilderImpl`) — purpose: marker base for FreeBSD builds. Inherits ctors via `using DerivationBuilderImpl::DerivationBuilderImpl;`.
- `ChrootFreeBSDDerivationBuilder` (struct, inherits `ChrootDerivationBuilder` and `FreeBSDDerivationBuilder`) — purpose: full chroot+jail builder for FreeBSD. Public field: `autoDelJail` (`std::shared_ptr<AutoRemoveJail>`, default-constructed). Ctor explicitly initialises the `DerivationBuilderImpl` virtual base, then `ChrootDerivationBuilder`, then `FreeBSDDerivationBuilder`.
  - Override `cleanupBuild(bool force)` — `autoDelJail->remove()`, then `ChrootDerivationBuilder::cleanupBuild(force)`.
  - Override `prepareSandbox()` — call `ChrootDerivationBuilder::prepareSandbox`, build `users` vector with root/nixbld/nobody entries, call `createPasswordFiles`, write `/etc/group`, create `/dev` (devfs ruleset 4) and `/bin` directories, mount devfs via `nmount`, then for every path in `pathsInChroot` set up nullfs mounts (read-only/`MNT_NOSUID` for in-store paths), and copy `/etc/nsswitch.conf`, `resolv.conf`, `services`, `hosts`, optional CA file for non-sandboxed FODs.
  - Override `startChild()` — for sandboxed: create jail with own `vnet`, then a helper subprocess uses netlink (snl) to bring up loopback; for non-sandboxed: create jail inheriting ip4/ip6, allowing raw sockets. Then `startProcess` calls `openSlave` and `runChild`.
  - Override `enterChroot()` — `closeExtraFDs`, `jail_attach(autoDelJail->jid)`.
  - `addDependency(const StorePath &)` — throws `UnimplementedError` (recursive-nix not supported).

### Functions
- `static void serializeString(std::vector<uint8_t> &, std::string const &)` / `static void serializeInt(std::vector<uint8_t> &, uint32_t)` — kind: file-local static, purpose: helpers writing the FreeBSD password DB binary format (big endian).
- `static std::vector<uint8_t> byNameKey(std::string const &)`, `byNumKey(uint32_t)`, `byUidKey(uid_t)` — kind: file-local static, purpose: build the password DB primary keys (`_PW_KEYBYNAME`, `_PW_KEYBYNUM`, `_PW_KEYBYUID`) using `_PW_VERSIONED`.
- `static void createPasswordFiles(std::filesystem::path & chrootRootDir, std::vector<PasswordEntry> & users)` — kind: file-local static, purpose: build `/etc/pwd.db` (binary `DB_HASH`) and a textual `/etc/passwd`.
- `template<size_t N> struct iovec iovFromMutableBuffer(std::array<char, N> &)`, `template<size_t N> struct iovec iovFromStaticSizedString(const char (&)[N])`, `struct iovec iovFromDynamicSizeString(const std::string &)` — kind: free function templates / function, purpose: build `iovec`s for FreeBSD `nmount` calls.

### Type aliases
- `using UniqueDB = std::unique_ptr<::DB, decltype([](::DB * db) { if (db) (db->close)(db); })>` — type alias inside the anonymous namespace, purpose: RAII wrapper around a Berkeley DB handle.

### Macros / globals
- `static constexpr HASHINFO dbFlags{ .bsize = 4096, .ffactor = 32, .nelem = 256, .cachesize = 2 * 1024 * 1024, .hash = nullptr, .lorder = BIG_ENDIAN }` — Berkeley-DB hash configuration for the password DB (anonymous-namespace).
- `static const uint8_t dbVersion = 4;` — password DB version (anonymous-namespace).

## File: src/libstore/unix/build/hook-instance.cc

### Namespaces
- `nix`.

### Classes / structs / enums
(none)

### Functions
- `HookInstance::HookInstance(const Strings & buildHook)` — kind: ctor, purpose: parse build-hook command, locate the hook executable via `ExecutablePath::load().findPath`, create `fromHook`, `toHook`, `builderOut` pipes, fork the hook process (`commonChildInit`, dup `fromHook.writeSide` to stderr, dup `toHook.readSide` to stdin, fd 4 for `builderOut.writeSide`, fd 5 for `builderOut.readSide`), set `SIGTERM` kill signal with 500ms timeout, `setSeparatePG`, push initial settings (`globalConfig.getSettings`) into the sink as a stream of `<1, name, value>` records terminated by `0`.
- `HookInstance::~HookInstance()` — kind: dtor, purpose: close the to-hook write side, kill the pid, run optional `onKillChild` callback. Swallows exceptions.

### Type aliases
(none)

### Macros / globals
- `using namespace std::chrono_literals;` — file-scope using-directive (allows `500ms` literal).

## File: src/libstore/unix/include/nix/store/build/hook-instance.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `HookInstance` (struct) — purpose: long-lived build-hook child process. Fields: `toHook`, `fromHook`, `builderOut` (all `Pipe`), `pid` (`Pid`), `machineName` (`std::string`, empty when owned by `Worker`, set when owned by a `Goal`), `sink` (`FdSink`), `activities` (`std::map<ActivityId, Activity>`), `onKillChild` (`std::function<void()>`, used to call `Worker::childTerminated`). Public ctor `(const Strings &)` and dtor.

### Functions
(only `HookInstance` ctor/dtor declarations)

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/unix/build/linux-derivation-builder.cc

### Namespaces
- `nix` (included into `derivation-builder.cc`; gated by `#ifdef __linux__`).

### Classes / structs / enums
- `LinuxDerivationBuilder` (struct, virtual inherits `DerivationBuilderImpl`) — purpose: Linux base builder: enforces `PR_SET_NO_NEW_PRIVS`, seccomp, optional landlock, and personality tweaks. Inherits ctors via `using`.
  - Override `enterChroot()` — set `PR_SET_NO_NEW_PRIVS`, call `setupSeccomp(localSettings)`, optionally `setupLandlock` (catching `EPERM`), call `linux::setPersonality({.system = drv.platform, .impersonateLinux26 = ...})`.
- `ChrootLinuxDerivationBuilder` (struct, inherits `ChrootDerivationBuilder` and `LinuxDerivationBuilder`) — purpose: full namespaced sandbox build. Public fields: `userNamespaceSync` (`Pipe`), `sandboxMountNamespace` (`AutoCloseFD`), `sandboxUserNamespace` (`AutoCloseFD`), `usingUserNamespace` (`bool`, default `true`), `cgroup` (`std::optional<std::filesystem::path>`). Ctor explicitly initialises the `DerivationBuilderImpl` virtual base.
  - `sandboxUid()` (non-virtual helper) — returns 1000/0/`buildUser->getUID()` depending on namespace usage and UID count.
  - Override `sandboxGid()` — analogous gid logic, falling back to `ChrootDerivationBuilder::sandboxGid()` when not using user namespaces.
  - Override `getBuildUser()` — call `acquireUserLock` with `useUidRange ? 65536 : 1` UID range and `useChroot=true`.
  - Override `prepareUser()` — when uid-range or `useCgroups`, require `Xp::Cgroups`, allocate a cgroup directory derived from `linux::getCgroupFS()` and `getRootCgroup()`, kill leftover cgroups for the build user via `nixStateDir/cgroups/<uid>`, then call base `prepareUser()`.
  - Override `prepareSandbox()` — call base, create the cgroup directory, chown the cgroup directory and `cgroup.procs`/`cgroup.threads` to the builder.
  - Override `startChild()` — pre-resolve AWS creds (when enabled), create `userNamespaceSync` pipe and `sendPid` pipe, fork a helper process that calls `openSlave`, drops supplementary groups (with `requireDropSupplementaryGroups` honoured), then `clone()`s with `CLONE_NEWPID|NEWNS|NEWIPC|NEWUTS|CLONE_PARENT|SIGCHLD` (plus `CLONE_NEWNET` if sandboxed and `CLONE_NEWUSER` if user-namespacing) and writes the PID back; the parent reads the child PID, writes `/proc/<pid>/{uid_map,gid_map,setgroups}`, writes `/etc/passwd` and `/etc/group` inside the chroot, opens and stores the saved mount/user namespace fds, moves the child into its cgroup, and signals the user-namespace-sync pipe. Throws if user namespaces are unavailable and no build user is configured.
  - Override `enterChroot()` — wait for the user-namespace-sync, configure loopback (sandboxed only) via `SIOCSIFFLAGS`, `sethostname`/`setdomainname`, mount `/` private (`MS_PRIVATE | MS_REC`), bind-mount the chroot to itself, bind-mount and `MS_SHARED` the sandbox store, create a minimal `/dev` (with optional `/dev/kvm` when `kvm` system feature is set and `kvm` is available), do additional copies for resolv.conf/services/hosts and optional CA file mount for non-sandboxed FODs, bind every path in `pathsInChroot` (with embedded sandbox shell support via `HAVE_EMBEDDED_SANDBOX_SHELL`), mount `/proc`, `/sys` (when uid-range), tmpfs `/dev/shm`, devpts, make `/etc` unwritable (unless uid-range), `unshare(CLONE_NEWNS)` (saving namespace state for `addDependency`), `unshare(CLONE_NEWCGROUP)` if cgroup, perform `pivot_root`/`chroot`/`umount2`/`rmdir real-root`; finally call `LinuxDerivationBuilder::enterChroot()`.
  - Override `setUser()` — `preserveDeathSignal([&] { setgid(sandboxGid()); setuid(sandboxUid()); })`.
  - Override `unprepareBuild()` — release saved namespace fds (`sandboxMountNamespace = -1`, `sandboxUserNamespace = -1`), then delegate to `DerivationBuilderImpl::unprepareBuild()`.
  - Override `killSandbox(bool getStats)` — when cgroup, `linux::destroyCgroup(*cgroup)` and capture `cpuUser`/`cpuSystem` into `buildResult`; otherwise fall back to base.
  - Override `addDependencyImpl(const StorePath &)` — fork a helper that `setns`s into the saved user namespace (if any) and then the saved mount namespace, then calls `doBind(source, target)`.

### Functions
- `static void setupSeccomp(const LocalSettings &)` — kind: file-local static, purpose: install seccomp filter (under `HAVE_SECCOMP`) blocking setuid/setgid bits via `chmod`, `fchmod`, `fchmodat`, `fchmodat2` (`NIX_SYSCALL_FCHMODAT2`) and forbidding xattr syscalls (returning `ENOTSUP`); arch-adds `SCMP_ARCH_X86`/`X32`/`ARM`/`MIPS*` based on `NIX_LOCAL_SYSTEM`. Throws `Error` otherwise unless `filterSyscalls` is disabled.
- `static int landlockCreateRuleset(const ::landlock_ruleset_attr *, std::size_t, std::uint32_t)`, `static int landlockRestrictSelf(Descriptor, std::uint32_t)`, `static int getLandlockAbiVersion()`, `static void setupLandlock()` — kind: file-local static (under `HAVE_LANDLOCK && defined(LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET)`), purpose: best-effort landlock sandbox restricting abstract unix sockets (ABI ≥ 6 required).
- `static void doBind(const std::filesystem::path & source, const std::filesystem::path & target, bool optional = false)` — kind: file-local static, purpose: bind-mount a host path into the chroot (handles directories with `MS_BIND | MS_REC`, copies symlinks, creates an empty file then bind-mounts for regular files).

### Type aliases
(none)

### Macros / globals
- `#define pivot_root(new_root, put_old) (syscall(SYS_pivot_root, new_root, put_old))` — wraps the syscall directly.
- `#define DO_LANDLOCK 1` (when landlock + abstract socket scope available) or `#define DO_LANDLOCK 0` — controls landlock setup. `#undef DO_LANDLOCK` at end of file.
- `static const std::filesystem::path procPath = "/proc";` — namespace-scope global.

## File: src/libstore/linux/personality.cc

### Namespaces
- `nix::linux`.

### Classes / structs / enums
(none)

### Functions
- `setPersonality(PersonalityArgs)` — kind: free function, purpose: set 32-bit personality (`PER_LINUX32`) on i686/armv7l/armv6l/armv5tel (or i686 cross-emulating x86_64), apply `UNAME26` (`0x0020000`) for Linux 2.6 impersonation when requested on i686/x86_64, and disable address-space randomisation via `ADDR_NO_RANDOMIZE`.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/linux/include/nix/store/personality.hh

### Namespaces
- `nix::linux`.

### Classes / structs / enums
- `PersonalityArgs` (struct) — purpose: parameter pack for `setPersonality`. Fields: `system` (`std::string_view`), `impersonateLinux26` (`bool`).

### Functions
- `setPersonality(PersonalityArgs)` — declaration.

### Type aliases
(none)

### Macros / globals
(none)

## File: src/libstore/linux/fchmodat2-compat.hh

### Namespaces
(none — purely macros)

### Classes / structs / enums
(none)

### Functions
(none)

### Type aliases
(none)

### Macros / globals
- `NIX_SYSCALL_FCHMODAT2` — platform-specific syscall number for `fchmodat2` (562 on `__alpha__`, 1073742276 on x32, 5452 on mips64/n64, 6452 on mips64/n32, 4452 on mips32, otherwise 452); only defined when `HAVE_SECCOMP`.

## Cross-file observations

### Goal class hierarchy
- All concrete goals derive from `Goal` and override `key()` (string identifying the goal in the Worker maps and ordering) plus `jobCategory()`. The job-category mapping is:
  - `JobCategory::Build` — `DerivationBuildingGoal`.
  - `JobCategory::Substitution` — `PathSubstitutionGoal`, `DrvOutputSubstitutionGoal`.
  - `JobCategory::Administration` — `DerivationTrampolineGoal`, `DerivationGoal`, `DerivationResolutionGoal` (purely orchestration goals).
- The key prefixes are: `"da$"` (`DerivationTrampolineGoal`), `"db$"` (`DerivationGoal`), `"dc$"` (`DerivationResolutionGoal`), `"dd$"` (`DerivationBuildingGoal`), `"a$"` (both `PathSubstitutionGoal` and `DrvOutputSubstitutionGoal`).
- Every goal stores its main work as a coroutine `Goal::Co` returned from a private/protected method; the constructor passes that coroutine to the `Goal(Worker &, Co init)` base ctor (which sets `top_co.handle.promise().goal = this`). Common shapes: `init` for substitution goals; `gaveUpOnSubstitution`/`tryToBuild`/`buildWithHook`/`buildLocally` for `DerivationBuildingGoal`; `haveDerivation`/`repairClosure` for `DerivationGoal`; `resolveDerivation` for `DerivationResolutionGoal`; `init`/`haveDerivation` for `DerivationTrampolineGoal`.
- Repeated counter-management pattern: `mcExpectedBuilds`/`mcRunningBuilds` (`MaintainCount<uint64_t>`) on `DerivationGoal` and `DerivationBuildingGoal`, `maintainExpectedSubstitutions`/`maintainRunningSubstitutions`/`maintainExpectedNar`/`maintainExpectedDownload` on `PathSubstitutionGoal`. All goals define `doneSuccess` and `doneFailure` that release these `MaintainCount<uint64_t>` handles, increment global counters on `worker` (e.g. `worker.doneBuilds++`, `worker.failedBuilds++`, `worker.doneSubstitutions++`), `worker.exitStatusFlags.updateFromStatus(ex.status)` for failures, then forward to `Goal::doneSuccess`/`Goal::doneFailure`. `DerivationBuildingGoal::doneSuccess` and `DerivationGoal::doneSuccess` share this pattern verbatim with only the counter-handle name differing; `PathSubstitutionGoal::cleanup` performs the mirror image of the substitution counter teardown via `worker.childTerminated(this, JobCategory::Substitution)`.
- Both `DerivationGoal::checkPathValidity` and `DerivationBuildingGoal::checkPathValidity` independently consult `worker.evalStore` and `worker.store` and (under `Xp::CaDerivations`) opportunistically register realisations. The two routines diverge in shape: one returns `std::optional<std::pair<UnkeyedRealisation, PathStatus>>` for a single output, the other a `std::pair<bool, SingleDrvOutputs>` for all outputs of a derivation. Both contain the same retry logic and CA-derivations registration backfill.
- `Worker::removeGoal` mirrors the per-goal-kind maps (one branch per concrete goal type via `dynamic_pointer_cast`). Every map (`derivationGoals`, `derivationResolutionGoals`, `derivationBuildingGoals`, `substitutionGoals`, `drvOutputSubstitutionGoals`, `derivationTrampolineGoals`) has paired insertion logic in `Worker::make*Goal` (via `initGoalIfNeeded`) and matching deletion logic in `removeGoal` — `derivationTrampolineGoals` uses the `DerivedPathMap` `ensureSlot`/`removeSlot` API rather than direct map operations.
- `Worker::initGoalIfNeeded` is the only place the goal cache is populated; all `make*Goal` factories go through it.

### DerivationBuilder hierarchy
- `DerivationBuilder` (abstract, in public header) — three pure virtuals: `startBuild`, `unprepareBuild`, `killChild`. Inherits `RestrictionContext` (which provides `state_` for tracking added paths/outputs).
- `DerivationBuilderImpl` (in `unix/build/derivation-builder.cc`) inherits both `DerivationBuilder` and `DerivationBuilderParams`; provides every concrete piece including outputs registration (`registerOutputs`), recursive-nix daemon (`startDaemon`/`stopDaemon`), env initialisation (`initEnv`), log handling (delegated to `DerivationBuilderCallbacks`), and the high-level `startBuild`/`unprepareBuild`. // FIXME comment marks it for renaming to `UnixDerivationBuilder`.
- `ChrootDerivationBuilder` (gated on Linux/FreeBSD) virtually inherits `DerivationBuilderImpl` and adds chroot-specific overrides (`setBuildTmpDir`, `tmpDirInSandbox`, `prepareSandbox`, `cleanupBuild`, `realPathInHost`, `getPreBuildHookArgs`, `needsHashRewrite() = false`, `addDependencyPrep` helper, virtual `sandboxGid`).
- `LinuxDerivationBuilder` virtually inherits `DerivationBuilderImpl` and overrides only `enterChroot` (seccomp/landlock/personality).
- `FreeBSDDerivationBuilder` virtually inherits `DerivationBuilderImpl` (no overrides; just a marker for diamond resolution).
- `ChrootLinuxDerivationBuilder` inherits `ChrootDerivationBuilder` + `LinuxDerivationBuilder`; adds user-namespace, cgroup, pivot_root, addDependency logic.
- `ChrootFreeBSDDerivationBuilder` inherits `ChrootDerivationBuilder` + `FreeBSDDerivationBuilder`; adds jail+nullfs+devfs setup, network configuration via netlink/`jail_setv`, password DB construction.
- `DarwinDerivationBuilder` inherits `DerivationBuilderImpl` directly; uses `sandbox_init_with_parameters` and Rosetta-aware `posix_spawn` with `posix_spawnattr_setbinpref_np`.
- `ExternalDerivationBuilder` inherits `DerivationBuilderImpl` directly; replaces `startChild` with a JSON-driven external program invocation; requires `Xp::ExternalBuilders` experimental feature.
- `DerivationBuilderImpl` is included as a single TU through `#include "chroot-derivation-builder.cc"`, `linux-derivation-builder.cc`, `freebsd-derivation-builder.cc`, `darwin-derivation-builder.cc`, `external-derivation-builder.cc` directives at the bottom of `unix/build/derivation-builder.cc`. As a result every platform builder lives in the same object file and all instances of `DerivationBuilderImpl` share the same linker symbol; the `chroot-derivation-builder.cc` / `linux-derivation-builder.cc` / etc. files cannot stand alone.
- Repeated overrides across `DerivationBuilder` subclasses:
  - `setBuildTmpDir()` — unchanged in `DerivationBuilderImpl` (sets `tmpDir = topTmpDir`); reset to `topTmpDir / "build"` in both `ChrootDerivationBuilder` and `ExternalDerivationBuilder`.
  - `tmpDirInSandbox()` — base returns `topTmpDir`; chroot returns the configured `sandboxBuildDir`; external builder hardcodes `"/build"`.
  - `prepareSandbox()` — base errors out if `useUidRange`; `ChrootDerivationBuilder` composes `BuildChrootParams` and calls `setupBuildChroot`, populating `pathsInChroot` from input closure; `DarwinDerivationBuilder` populates `pathsInChroot` only; `ChrootFreeBSDDerivationBuilder` adds password DB + devfs + nullfs mounts on top; `ChrootLinuxDerivationBuilder` adds cgroup creation on top.
  - `enterChroot()` — base no-op; `LinuxDerivationBuilder` applies seccomp + landlock + personality; `ChrootLinuxDerivationBuilder` performs the full namespace+pivot_root dance and then chains to `LinuxDerivationBuilder::enterChroot`; `ChrootFreeBSDDerivationBuilder` does `closeExtraFDs` + `jail_attach`.
  - `setUser()` — base sets supplementary groups + gid + uid (under `preserveDeathSignal`); Linux chroot uses the namespaced sandbox uid/gid (without supplementary groups); Darwin reuses the base implementation but then synthesises and applies the sandbox profile.
  - `startChild()` — base just `startProcess` with `openSlave` + `runChild`; `ChrootLinuxDerivationBuilder` uses a helper-process to clone with namespaces; `ChrootFreeBSDDerivationBuilder` creates a jail (sandboxed: own vnet + manual loopback config; non-sandboxed: inherit ip4/ip6); `ExternalDerivationBuilder` writes JSON and execs the external program.
  - `cleanupBuild()` — base deletes redirected outputs and `topTmpDir`; chroot variants additionally move outputs out of the chroot or destroy the jail (FreeBSD).
  - `killSandbox()` — base kills the build user (`killUser(uid)`); Linux destroys the cgroup and harvests CPU stats into `buildResult`; Darwin additionally cleans up SysV IPC objects via `cleanupSysVIPCForUser`.
  - `getBuildUser()` — base uses 1-UID lock without chroot; `ChrootLinuxDerivationBuilder` requests `useUidRange ? 65536 : 1` UIDs with chroot.
- The `DerivationBuilderCallbacks` interface (declared in the public header) is used as a delegation seam from `DerivationBuildingGoal::buildLocally` (concrete subclass `DerivationBuildingGoalCallbacks` defined as a local class inside the coroutine in `derivation-building-goal.cc`).
- `DerivationBuilderDeleter::operator()` performs a `dynamic_cast<DerivationBuilderImpl *>` to invoke `cleanupOnDestruction()` (which itself calls `killChild`/`stopDaemon`/`cleanupBuild(false)`) before `delete`-ing — virtual function calls from a destructor are unsafe, so the cleanup is hoisted out of the destructor into the deleter.

### Builtin builders
- All three builtin builders (`builtinBuildenv`, `builtinFetchurl`, `builtinUnpackChannel`) follow the same pattern: a static function with signature `void(const BuiltinBuilderContext &)` plus a file-scope `static RegisterBuiltinBuilder` instance to register it. Each looks up env attributes via a local `getAttr` lambda and writes the requested output via `ctx.outputs.at("out")`.
- `builtinFetchurl` is special-cased in `DerivationBuilderImpl::runChild`: when `drv.builder == "builtin:fetchurl"`, it pre-populates `ctx.netrcData` and `ctx.caFileData` from `fileTransferSettings` so that the in-sandbox fetch can write them to `tmpDirInSandbox`.

### Platform helpers
- `commonChildInit()` (child.cc) is invoked from every place that forks a builder: `runChild` (in `DerivationBuilderImpl`), `HookInstance::HookInstance`, `ExternalDerivationBuilder::startChild`. The setup is identical (simple logger + restoreProcessContext + setsid + dup stderr→stdout + stdin from /dev/null), suggesting it is the canonical pre-exec routine.
- `setupBuildChroot` (unix/build/chroot.cc) is the only path that creates the `*.chroot` directory; both `ChrootDerivationBuilder::prepareSandbox` (Linux & FreeBSD via the diamond) and the FreeBSD chroot extension build a `BuildChrootParams` to call into it.
- The `LogSink` defined in `derivation-building-goal.cc` (post-build hook) and `BuildLog` (build-log.hh/cc) both implement `Sink::operator()` as a line-buffer that splits on `\n`. `BuildLog` adds JSON parsing (`handleJSONLogMessage`), tail tracking, and `\r` carriage-return handling; `LogSink` is a much simpler version. They could share a base class.
- `Worker::childTerminated` has two overloads: the `(Goal *)` overload calls `goal->jobCategory()` (only safe when the goal is a fully-constructed object), the `(Goal *, JobCategory)` overload takes the category explicitly so that destructors of partially-destroyed goals can call it (this is used by `PathSubstitutionGoal::cleanup` and `DerivationBuildingGoalCallbacks::childTerminated`).
- Both `derivation-building-goal.cc` and `worker.cc` perform identical post-substitution/build counter book-keeping; the differences are minor and centred on which `MaintainCount<uint64_t>` instance maintains the work-in-progress state.
- `JobCategory` switch statements appear in: `Worker::childStarted`, `Worker::childTerminated(Goal *, JobCategory)`, and `Worker::waitForBuildSlot`. The pattern is duplicated rather than centralised behind a helper. The `Administration` arm is `unreachable` in `childStarted`/`childTerminated`.
- The `Goal::Co` coroutine machinery in goal.hh/goal.cc is bespoke — `await_transform` overloads handle `Co`, `Suspend`, `WaitForChildEvent`, and `AsyncCallback<T>`. The `AsyncCallback` plumbing in `goal-impl.hh` is the only piece that crosses thread boundaries (via `Worker::Waker::enqueue`).
- The wakeup mechanism is asymmetric across platforms: POSIX uses a `unix::SelfPipe` polled alongside child fds; Windows uses an I/O completion port via `PostQueuedCompletionStatus`. `Worker::Waker::wakeAll` drains both forms and erases woken goals from `waitingForCompletion` while moving them to `awake` via `wakeUp`.
