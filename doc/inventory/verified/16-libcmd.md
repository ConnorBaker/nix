# Inventory — Shard 16: libcmd

## File: src/libcmd/built-path.cc

### Namespaces
- `nix` — wraps all definitions.

### Classes / structs / enums
None defined here (definitions are header-side; this file provides member function bodies for `SingleBuiltPath`, `SingleBuiltPathBuilt`, `BuiltPath`, `BuiltPathBuilt`).

### Functions
- `nix::SingleBuiltPath::outPath` — member function; returns the single output `StorePath` by visiting the variant.
- `nix::BuiltPath::outPaths` — member function; returns the set of output `StorePath`s by visiting the variant.
- `nix::SingleBuiltPath::Built::discardOutputPath` — member function; downgrades a built path to a `SingleDerivedPath::Built` without the resolved output path.
- `nix::SingleBuiltPath::discardOutputPath` — member function; visit-dispatch wrapper that downgrades the variant to a plain `SingleDerivedPath`.
- `nix::BuiltPath::Built::toJSON` — member function; serialises a multi-output built path to JSON keyed by output name.
- `nix::SingleBuiltPath::Built::toJSON` — member function; serialises a single-output built path to JSON.
- `nix::SingleBuiltPath::toJSON` — member function; visit-dispatch JSON serialiser; opaque variant prints store path string, built variant delegates.
- `nix::BuiltPath::toJSON` — member function; visit-dispatch JSON serialiser; opaque variant prints store path string, built variant delegates.
- `nix::BuiltPath::toRealisedPaths` — member function; expands a built path to a `RealisedPath::Set` by calling `deepQueryPartialDerivationOutput` per output and inserting realisations + the output path itself.

### Type aliases
None.

### Macros / globals
- `GENERATE_CMP_EXT(, std::strong_ordering, SingleBuiltPathBuilt, *me->drvPath, me->output)` — macro instantiation generating comparison operators for `SingleBuiltPathBuilt` (custom to avoid `ref` pointer equality).
- `GENERATE_EQUAL(, BuiltPathBuilt::, BuiltPathBuilt, *me->drvPath, me->outputs)` — macro instantiation generating equality for `BuiltPathBuilt`; ordering elided due to libc++ 16 (Darwin) missing `std::map::operator<=>`.

## File: src/libcmd/command-installable-value.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::InstallableValueCommand::run(ref<Store>, ref<Installable>)` — override that downcasts to `InstallableValue` via `InstallableValue::require` and forwards to the typed `run`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/command.cc

### Namespaces
- `nix`

### Classes / structs / enums
None defined here (the file provides member-function bodies and a few constructors).

### Functions
- `nix::RegisterCommand::commands` — static accessor returning the singleton `Commands` map of registered commands.
- `nix::RegisterLegacyCommand::commands` — static accessor returning the singleton `Commands` map of legacy commands.
- `nix::RegisterCommand::getCommandsFor` — static; returns subcommands matching a given name prefix (filtering registry entries whose path is exactly one element longer than the prefix).
- `nix::NixMultiCommand::toJSON` — override that delegates to `MultiCommand::toJSON` (FIXME notes that `Command::toJSON` should also be used).
- `nix::NixMultiCommand::run` — override that requires a sub-command, otherwise throws a `UsageError` listing available sub-commands as Markdown rendered via `renderMarkdownToTerminal`.
- `nix::StoreConfigCommand::StoreConfigCommand` — default constructor (empty body).
- `nix::StoreConfigCommand::getStoreConfig` — memoised accessor; lazily calls `createStoreConfig`.
- `nix::StoreConfigCommand::createStoreConfig` — virtual factory; resolves the store config from `settings.storeUri` via `resolveStoreConfig`.
- `nix::StoreConfigCommand::run()` — override; calls `run(getStoreConfig())`.
- `nix::StoreCommand::StoreCommand` — default constructor (empty body).
- `nix::StoreCommand::getStore` — memoised accessor; lazily calls `createStore`.
- `nix::StoreCommand::createStore` — opens the store from `getStoreConfig()->openStore()` and calls `init`.
- `nix::StoreCommand::run(ref<StoreConfig>)` — override that asserts the passed config matches `getStore()`'s config and forwards to typed `run`.
- `nix::CopyCommand::CopyCommand` — adds `--from`/`--to` flags that parse store URIs into `srcUri`/`dstUri`.
- `nix::CopyCommand::createStoreConfig` — override; uses `srcUri` if set, else delegates to `StoreCommand::createStoreConfig`.
- `nix::CopyCommand::getDstStore` — opens the destination store, requiring at least one of `--from`/`--to`.
- `nix::EvalCommand::EvalCommand` — adds `--debugger` flag that toggles `startReplOnEvalErrors`.
- `nix::EvalCommand::~EvalCommand` — destructor; emits eval stats via `maybePrintStats` if a state was created.
- `nix::EvalCommand::getEvalStore` — memoised accessor; opens the eval store URL or falls back to the regular store.
- `nix::EvalCommand::getEvalState` — memoised accessor; constructs an `EvalState` with `traceable_allocator`, propagates `repair`, and wires `evalState->debugRepl` to `AbstractNixRepl::runSimple` if `startReplOnEvalErrors`.
- `nix::MixOperateOnOptions::MixOperateOnOptions` — adds the `--derivation` flag that switches `operateOn` to `Derivation`.
- `nix::BuiltPathsCommand::BuiltPathsCommand` — constructor; adds either `--no-recursive` (when default `recursive=true`) or `--recursive`/`-r` (when default `false`), and unconditionally `--all`.
- `nix::BuiltPathsCommand::run(ref<Store>, Installables &&)` — override; either enumerates all valid paths (`--all`), or calls `Installable::toBuiltPaths`, optionally computes FS closure when `recursive`, then forwards to typed `run`.
- `nix::StorePathsCommand::StorePathsCommand` — constructor; forwards `recursive` to `BuiltPathsCommand`.
- `nix::StorePathsCommand::run(ref<Store>, BuiltPaths &&, BuiltPaths &&)` — override; flattens to a topo-sorted reversed list of `StorePath`s and forwards.
- `nix::StorePathCommand::run(ref<Store>, StorePaths &&)` — override; asserts exactly one store path then forwards.
- `nix::MixProfile::MixProfile` — adds the `--profile` flag with `completePath` completer.
- `nix::MixProfile::updateProfile(Store &, const StorePath &)` — updates a profile symlink to a single store path; requires `LocalFSStore`; uses `switchLink`/`createGeneration`.
- `nix::MixProfile::updateProfile(Store &, const BuiltPaths &)` — flattens built paths to one store path and delegates.
- `nix::MixDefaultProfile::MixDefaultProfile` — sets `profile` to `getDefaultProfile(settings.getProfileDirsOptions())`.
- `nix::MixEnvironment::MixEnvironment` — initialiser sets `ignoreEnvironment(false)`; adds `--ignore-env`/`-i` (alias `ignore-environment`), `--keep-env-var`/`-k` (alias `keep`), `--unset-env-var`/`-u` (alias `unset`) with mutual-exclusion check vs `setVars`, `--set-env-var`/`-s` with mutual-exclusion vs `unsetVars` and duplicate-set check.
- `nix::MixEnvironment::setEnviron` — applies environment mutations to the global environ via `getEnv`/`replaceEnv`; throws if `--unset-env-var` combined with `--ignore-env`, or `--keep-env-var` without `--ignore-env`.
- `nix::createOutLinks` — free function; creates result symlinks for built paths against a `LocalFSStore` (handling indices for `Opaque` and per-output names for `Built`, suppressing `-out` suffix when output is `out`).
- `nix::MixOutLinkBase::createOutLinksMaybe` — calls `createOutLinks` if `outLink` is non-empty and the store is a `LocalFSStore`.

### Type aliases
None.

### Macros / globals
- `static constexpr auto environmentVariablesCategory` — file-scope constant; flag-category label "Options that change environment variables".

## File: src/libcmd/common-eval-args.cc

### Namespaces
- `nix`

### Classes / structs / enums
None defined here.

### Functions
- `nix::MixEvalArgs::MixEvalArgs` — adds eval flags: `--arg`, `--argstr`, `--arg-from-file`, `--arg-from-stdin`, `--include`/`-I`, `--impure`, `--override-flake`, `--eval-store`.
- `nix::MixEvalArgs::getAutoArgs` — builds `Bindings` from the accumulated `autoArgs` map by visiting the variant kinds (`AutoArgExpr` parses an expression thunk respecting `nixShellShebangArgumentsRelativeToScript`, `AutoArgString` makes a string value, `AutoArgFile` reads from path, `AutoArgStdin` reads from STDIN).
- `nix::lookupFileArg` — free function; resolves a string argument to a `SourcePath`, handling pseudo-URLs (downloads tarball), `flake:` refs (requires Flakes feature, lazy-fetches), `<lookup-paths>` (`state.findFile`), or raw filesystem paths (via `absPath` + `state.rootPath`).

### Type aliases
None at file scope.

### Macros / globals
- `nix::fetchSettings` — global `fetchers::Settings` instance.
- `static GlobalConfig::Register rFetchSettings(&fetchSettings)` — registers the fetch settings in `GlobalConfig`.
- `nix::evalSettings` — global `EvalSettings` instance, configured with a `flake:` lookup-path resolver lambda.
- `static GlobalConfig::Register rEvalSettings(&evalSettings)` — registers eval settings.
- `nix::flakeSettings` — global `flake::Settings` instance.
- `static GlobalConfig::Register rFlakeSettings(&flakeSettings)` — registers flake settings.
- `nix::compatibilitySettings` — global `CompatibilitySettings` instance.
- `static GlobalConfig::Register rCompatibilitySettings(&compatibilitySettings)` — registers compatibility settings.

## File: src/libcmd/editor-for.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::editorFor` — free function; constructs argv for `$EDITOR` (or `cat`) on a `SourcePath`, optionally adds `+lineno` for emacs/nano/vim/kak; if the source has no physical path it copies contents to a temp file (with same basename for syntax highlighting), respecting `readOnly` (0400 vs 0600). Returns argv plus RAII guards (`AutoCloseFD`, `AutoDelete`). Resolves symlinks before stat to avoid GitSourceAccessor symlink issues; throws if not a regular file.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/get-build-log.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::fetchBuildLog` — free function; iterates over the store and `getDefaultSubstituters()`, casting each to `LogStore`; logs an info message and skips non-LogStore subs; calls `getBuildLog`; returns the first available build log; throws `Error` if none.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/built-path.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::SingleBuiltPathBuilt` — struct; holds `ref<SingleBuiltPath> drvPath` and `std::pair<std::string, StorePath> output`. Provides `discardOutputPath` (returns `SingleDerivedPathBuilt`), `to_string`, static `parse`, `toJSON`, `operator==` (noexcept), `operator<=>` (noexcept, `std::strong_ordering`).
- `nix::SingleBuiltPath` — struct; inherits from `_SingleBuiltPathRaw = std::variant<DerivedPathOpaque, SingleBuiltPathBuilt>`. Inherits variant ctors via `using Raw::Raw;`. Aliases `Raw`, `Opaque = DerivedPathOpaque`, `Built = SingleBuiltPathBuilt`. Provides defaulted `operator==`/`operator<=>`, inline `raw`, `outPath`, `discardOutputPath`, static `parse`, `toJSON`.
- `nix::BuiltPathBuilt` — struct; holds `ref<SingleBuiltPath> drvPath` and `std::map<std::string, StorePath> outputs`. Provides `operator==` (noexcept); `operator<=>` commented out due to libc++ 16 missing `std::map::operator<=>`. Provides `to_string`, static `parse`, `toJSON`.
- `nix::BuiltPath` — struct; inherits from `_BuiltPathRaw = std::variant<DerivedPath::Opaque, BuiltPathBuilt>`. Inherits variant ctors via `using Raw::Raw;`. Aliases `Raw`, `Opaque = DerivedPathOpaque`, `Built = BuiltPathBuilt`. Defaulted `operator==` only (no `operator<=>` due to libc++ 16). Inline `raw`, `outPaths`, `toRealisedPaths`, `toJSON`.
- Forward declaration of `SingleBuiltPath` precedes the definitions.

### Functions
- `nix::staticDrv` — `static inline` free helper; wraps a `StorePath` into a `ref<SingleBuiltPath>` of the `Opaque` alternative.

### Type aliases
- `nix::_SingleBuiltPathRaw = std::variant<DerivedPathOpaque, SingleBuiltPathBuilt>`
- `nix::_BuiltPathRaw = std::variant<DerivedPath::Opaque, BuiltPathBuilt>`
- `nix::SingleBuiltPath::Raw`
- `nix::SingleBuiltPath::Opaque` (= `DerivedPathOpaque`)
- `nix::SingleBuiltPath::Built` (= `SingleBuiltPathBuilt`)
- `nix::BuiltPath::Raw`
- `nix::BuiltPath::Opaque` (= `DerivedPathOpaque`)
- `nix::BuiltPath::Built` (= `BuiltPathBuilt`)
- `typedef std::vector<BuiltPath> BuiltPaths` — collection alias.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/command-installable-value.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::InstallableValueCommand` — struct, inherits `InstallableCommand`. Adds pure-virtual `run(ref<Store>, ref<InstallableValue>)`; overrides `run(ref<Store>, ref<Installable>)` to downcast.

### Functions
None beyond the struct above.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/command.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::NixMultiCommand` — struct; inherits `MultiCommand` and `virtual Command`. Inherits `MultiCommand`'s constructor via `using MultiCommand::MultiCommand;`. Overrides `toJSON` and `run`.
- `nix::StoreConfigCommand` — struct; inherits `virtual Command`. Holds `private std::shared_ptr<StoreConfig> _storeConfig`. Methods `getStoreConfig`, virtual `createStoreConfig`, override `run()`, pure `run(ref<StoreConfig>)`.
- `nix::StoreCommand` — struct; inherits `virtual StoreConfigCommand`. Holds `private std::shared_ptr<Store> _store`. Methods `getStore`, `createStore`, override `run(ref<StoreConfig>)`, pure `run(ref<Store>)`.
- `nix::CopyCommand` — struct; inherits `virtual StoreCommand`. Holds `std::optional<StoreReference> srcUri, dstUri`. Overrides `createStoreConfig`; adds `getDstStore`.
- `nix::EvalCommand` — struct; inherits `virtual StoreCommand`, `MixEvalArgs`. Holds `startReplOnEvalErrors = false`, `ignoreExceptionsDuringTry = false`, plus private memoised `evalStore`/`evalState` (`std::shared_ptr`). Methods `getEvalStore`, `getEvalState`. Has destructor.
- `nix::MixFlakeOptions` — struct; inherits `virtual Args`, `EvalCommand`. Holds `flake::LockFlags lockFlags`. Virtual `getFlakeRefsForCompletion` returning `{}`.
- `nix::SourceExprCommand` — struct; inherits `virtual Args`, `MixFlakeOptions`. Holds `std::optional<std::filesystem::path> file`, `std::optional<std::string> expr`. Methods `parseInstallables`, `parseInstallable`, virtual `getDefaultFlakeAttrPaths`/`getDefaultFlakeAttrPathPrefixes`, `completeInstallable`, `getCompleteInstallable`.
- `nix::MixReadOnlyOption` — struct; inherits `virtual Args`. Constructor only (declared, defined in installables.cc).
- `nix::RawInstallablesCommand` — struct; inherits `virtual Args`, `SourceExprCommand`. Pure `run(ref<Store>, std::vector<std::string> &&)`; overrides `run(ref<Store>)`. Virtual `applyDefaultInstallables` (FIXME re const). Holds `readFromStdIn = false`, private `rawInstallables`. Override `getFlakeRefsForCompletion`.
- `nix::InstallablesCommand` — struct; inherits `RawInstallablesCommand`. Pure `run(ref<Store>, Installables &&)`; overrides `run(ref<Store>, std::vector<std::string> &&)`.
- `nix::InstallableCommand` — struct; inherits `virtual Args`, `SourceExprCommand`. Constructor; pure `run(ref<Store>, ref<Installable>)`; overrides `run(ref<Store>)`. Holds `private std::string _installable{"."}`. Override `getFlakeRefsForCompletion`.
- `nix::MixOperateOnOptions` — struct; inherits `virtual Args`. Holds `OperateOn operateOn = OperateOn::Output`. Constructor.
- `nix::BuiltPathsCommand` — struct; inherits `InstallablesCommand`, `virtual MixOperateOnOptions`. Holds `private bool recursive = false; bool all = false;`, `protected Realise realiseMode = Realise::Derivation`. Constructor with optional `recursive` parameter. Pure `run(ref<Store>, BuiltPaths &&, BuiltPaths &&)`; overrides `run(ref<Store>, Installables &&)` and `applyDefaultInstallables`.
- `nix::StorePathsCommand` — struct (declared with `public BuiltPathsCommand` inheritance); pure `run(ref<Store>, StorePaths &&)`; overrides `run(ref<Store>, BuiltPaths &&, BuiltPaths &&)`. Constructor with optional `recursive`.
- `nix::StorePathCommand` — struct (declared with `public StorePathsCommand` inheritance). Pure `run(ref<Store>, const StorePath &)`; overrides `run(ref<Store>, StorePaths &&)`.
- `nix::RegisterCommand` — struct; central registry of `Command` factories keyed by name vector. Inner type `Commands = std::map<std::vector<std::string>, fun<ref<Command>()>>`. Methods `commands` (singleton), constructor inserts into the singleton via `emplace`, static `getCommandsFor`.
- `nix::MixProfile` — struct; inherits `virtual StoreCommand`. Holds `std::optional<std::filesystem::path> profile`. Constructor. Methods `updateProfile(Store &, const StorePath &)` and `updateProfile(Store &, const BuiltPaths &)`.
- `nix::MixDefaultProfile` — struct; inherits `MixProfile`. Default-initialises `profile` to the user default.
- `nix::MixEnvironment` — struct; inherits `virtual Args`. Holds `StringSet keepVars`, `StringSet unsetVars`, `StringMap setVars`, `bool ignoreEnvironment`. Constructor and `setEnviron`.
- `nix::MixNoCheckSigs` — struct; inherits `virtual Args`. Holds `CheckSigsFlag checkSigs = CheckSigs`. Inline constructor adds the `--no-check-sigs` flag.
- `nix::MixOutLinkBase` — struct; inherits `virtual Args`. Holds `std::filesystem::path outLink`. Constructor takes `defaultOutLink` and initialises `outLink`. Method `createOutLinksMaybe`.
- `nix::MixOutLinkByDefault` — struct; inherits `MixOutLinkBase`, `virtual Args`. Inline constructor passes `"result"` to base then adds `--out-link`/`-o` and `--no-link` flags.

### Functions
- `nix::registerCommand<T>` — `static` function template; constructs a `RegisterCommand` for a single-name command of type `T`.
- `nix::registerCommand2<T>` — `static` function template; same but accepts a vector of name segments.
- `nix::completeFlakeInputAttrPath` — free function declaration; completer for flake input attr paths.
- `nix::completeFlakeRef` — free function declaration; completer for flake refs.
- `nix::completeFlakeRefWithFragment` — free function declaration; completer that recognises `flakeref#fragment`.
- `nix::showVersions` — free function declaration; renders a sorted-version `StringSet`.
- `nix::printClosureDiff` — free function declaration; prints a closure diff between two store paths.
- `nix::createOutLinks` — free function declaration; creates result symlinks (impl in `command.cc`).

### Type aliases
- `nix::RegisterCommand::Commands = std::map<std::vector<std::string>, fun<ref<Command>()>>`

### Macros / globals
- `nix::programPath` — `extern std::string`; main program path.
- `nix::savedArgv` — `extern char **`; saved process argv.
- `nix::catHelp` — `static constexpr Command::Category` = -1.
- `nix::catSecondary` — `static constexpr Command::Category` = 100.
- `nix::catUtility` — `static constexpr Command::Category` = 101.
- `nix::catNixInstallation` — `static constexpr Command::Category` = 102.
- `nix::installablesCategory` — `static constexpr auto`; flag-category label string referencing `[installables]`.
- `#pragma GCC diagnostic ignored "-Woverloaded-virtual"` — disables the overloaded-virtual warning for the run-method overloading idiom.

## File: src/libcmd/include/nix/cmd/common-eval-args.hh

### Namespaces
- `nix` — top level.
- `nix::fetchers` — forward-declares `fetchers::Settings`.
- `nix::flake` — forward-declares `flake::Settings`.

### Classes / structs / enums
- `nix::MixEvalArgs` — struct; inherits `virtual Args`, `virtual MixRepair`. Holds `LookupPath lookupPath`, `std::optional<StoreReference> evalStoreUrl`, private `autoArgs` map. Provides `getAutoArgs` and the constructor.
- `nix::MixEvalArgs::AutoArgExpr` — private nested struct; holds `std::string expr`.
- `nix::MixEvalArgs::AutoArgString` — private nested struct; holds `std::string s`.
- `nix::MixEvalArgs::AutoArgFile` — private nested struct; holds `std::filesystem::path path`.
- `nix::MixEvalArgs::AutoArgStdin` — private nested empty struct.

### Functions
- `nix::lookupFileArg` — free function declaration with default `baseDir = nullptr` (impl in common-eval-args.cc).

### Type aliases
- `nix::MixEvalArgs::AutoArg = std::variant<AutoArgExpr, AutoArgString, AutoArgFile, AutoArgStdin>` (private).

### Macros / globals
- `nix::MixEvalArgs::category` — `static constexpr auto` flag-category label "Common evaluation options".
- `nix::fetchSettings` — `extern fetchers::Settings`.
- `nix::evalSettings` — `extern EvalSettings`.
- `nix::flakeSettings` — `extern flake::Settings`.
- `nix::compatibilitySettings` — `extern CompatibilitySettings`.

## File: src/libcmd/include/nix/cmd/compatibility-settings.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::CompatibilitySettings` — struct, inherits `Config`. Default constructor. Two settings:
  - `Setting<bool> nixShellAlwaysLooksForShellNix` (default `true`) — Nix 2.24 behavior toggle for `nix-shell` looking for `shell.nix` always.
  - `Setting<bool> nixShellShebangArgumentsRelativeToScript` (default `true`) — Nix 2.24 behavior toggle for shebang argument relative paths.

### Functions
None.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/editor-for.hh

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::editorFor` — free function declaration; helper to invoke `$EDITOR` on a `SourcePath` at a line; `readOnly` defaulted to `true`. Returns `std::tuple<OsStrings, AutoCloseFD, AutoDelete>`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/get-build-log.hh

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::fetchBuildLog` — free function declaration; fetch a build log from the store/substituters. Signature `std::string fetchBuildLog(ref<Store> store, const StorePath & path, std::string_view what)`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/installable-attr-path.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::InstallableAttrPath` — class (private inheritance via default class access); inherits `public InstallableValue`. Holds `SourceExprCommand & cmd`, `RootValue v`, `std::string attrPath`, `ExtendedOutputsSpec extendedOutputsSpec`. Private constructor; overrides `what` (inline returning `attrPath`), `toValue`, `toDerivedPaths`. Public static factory `parse`.

### Functions
None at file scope (members declared above).

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/installable-derived-path.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::InstallableDerivedPath` — struct; inherits `Installable`. Holds `ref<Store> store`, `DerivedPath derivedPath`. Inline constructor takes `ref<Store>` and `DerivedPath &&` (moves into member). Overrides `what`, `toDerivedPaths`, `getStorePath`. Public static factory `parse`.

### Functions
None at file scope (members declared above).

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/installable-flake.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::ExtraPathInfoFlake` — struct; inherits `ExtraPathInfoValue`. Inner struct `Flake { FlakeRef originalRef; FlakeRef lockedRef; }`. Holds `Flake flake`. Inline constructor takes `Value &&` and `Flake &&`.
- `nix::ExtraPathInfoFlake::Flake` — nested struct.
- `nix::InstallableFlake` — struct; inherits `InstallableValue`. Holds `FlakeRef flakeRef`, `Strings attrPaths`, `Strings prefixes`, `ExtendedOutputsSpec extendedOutputsSpec`, `const flake::LockFlags & lockFlags`, `mutable std::shared_ptr<flake::LockedFlake> _lockedFlake`. Constructor. Overrides `what` (inline), `toDerivedPaths`, `toValue`, `getCursors`. Adds `getActualAttrPaths`, `getLockedFlake() const`, `nixpkgsFlakeRef() const`.

### Functions
- `nix::defaultNixpkgsFlakeRef` — `static inline` free helper; returns the indirect `nixpkgs` flake reference via `FlakeRef::fromAttrs`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/installable-value.hh

### Namespaces
- `nix`
- `nix::eval_cache` — forward declarations of `EvalCache`, `AttrCursor`.

### Classes / structs / enums
- `nix::App` — struct; holds `std::vector<DerivedPath> context`, `std::filesystem::path program` (FIXME comment about extending fields).
- `nix::UnresolvedApp` — struct; holds `App unresolved`. Methods `build`, `resolve`.
- `nix::ExtraPathInfoValue` — struct; inherits `ExtraPathInfo`. Inner `Value { std::optional<NixInt::Inner> priority; std::string attrPath; ExtendedOutputsSpec extendedOutputsSpec; }`. Holds `Value value`. Inline constructor takes `Value &&`. Virtual destructor.
- `nix::ExtraPathInfoValue::Value` — nested struct.
- `nix::InstallableValue` — struct; inherits `Installable`. Holds `ref<EvalState> state`. Inline constructor; virtual destructor. Pure virtual `toValue`. Virtual `getCursors`, `getCursor`. Adds `toApp`. Static `require` (two overloads: `Installable &` and `ref<Installable>`). Protected helper `trySinglePathToDerivedPaths`.

### Functions
None at file scope beyond struct members.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/installables.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::Realise` — `enum class`: `Outputs`, `Derivation`, `Nothing`. Postcondition documented per-variant.
- `nix::OperateOn` — `enum class`: `Output`, `Derivation`. Selects what installables refer to.
- `nix::ExtraPathInfo` — struct; intentionally empty base, virtual destructor (defaulted).
- `nix::DerivedPathWithInfo` — struct; pairs a `DerivedPath path` with `ref<ExtraPathInfo> info`.
- `nix::BuiltPathWithResult` — struct; holds `BuiltPath path`, `ref<ExtraPathInfo> info`, `std::optional<BuildResult> result`.
- `nix::Installable` — struct; abstract base. Virtual destructor. Pure virtual `what`, `toDerivedPaths`. Concrete `toDerivedPath` (single-result wrapper). Virtual `getStorePath` (default returns `{}`). Static helpers `build`, `build2`, `toStorePathSet`, `toStorePaths`, `toStorePath`, `toDerivations` (with `useDeriver = false`), `toBuiltPaths`.

### Functions
- `nix::toBuiltPaths` (overload taking `const std::vector<BuiltPathWithResult> &`) — free function declaration; flattens to a `BuiltPaths`.

### Type aliases
- `nix::DerivedPathsWithInfo = std::vector<DerivedPathWithInfo>`
- `nix::Installables = std::vector<ref<Installable>>`

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/legacy.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::RegisterLegacyCommand` — struct; static `Commands & commands()`. Inline constructor inserts a name → `MainFunction` mapping via `insert_or_assign`.

### Functions
None at file scope.

### Type aliases
- `nix::MainFunction = fun<void(int, char **)>` (typedef)
- `nix::RegisterLegacyCommand::Commands = std::map<std::string, MainFunction>` (typedef)

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/markdown.hh

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::renderMarkdownToTerminal` — free function declaration; render markdown taking a `std::string_view`, returning `std::string` (returns input as-is when built without lowdown).

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/misc-store-flags.hh

### Namespaces
- `nix::flag`

### Classes / structs / enums
None.

### Functions
- `nix::flag::hashAlgo(std::string &&, HashAlgorithm *)` — overload generating an `Args::Flag` for a hash algorithm.
- `nix::flag::hashAlgo(HashAlgorithm *)` — `static inline` overload defaulting longName to `"hash-algo"`.
- `nix::flag::hashAlgoOpt(std::string &&, std::optional<HashAlgorithm> *)` — overload for optional hash algo.
- `nix::flag::hashAlgoOpt(std::optional<HashAlgorithm> *)` — `static inline` overload with default longName `"hash-algo"`.
- `nix::flag::hashFormatWithDefault(std::string &&, HashFormat *)` — flag for hash format that asserts SRI default.
- `nix::flag::hashFormatOpt(std::string &&, std::optional<HashFormat> *)` — flag for optional hash format.
- `nix::flag::fileIngestionMethod(FileIngestionMethod *)` — flag for `--mode` selecting NAR/flat ingestion.
- `nix::flag::contentAddressMethod(ContentAddressMethod *)` — flag for `--mode` selecting content-address method.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/network-proxy.hh

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::haveNetworkProxyConnection` — free function declaration; heuristic check based on env vars.

### Type aliases
None.

### Macros / globals
- `nix::networkProxyVariables` — `extern const StringSet`; full set of proxy-related env vars.

## File: src/libcmd/include/nix/cmd/repl-interacter.hh

### Namespaces
- `nix`
- `nix::detail` — wraps `ReplCompleterMixin` to keep the REPL's internals private.

### Classes / structs / enums
- `nix::detail::ReplCompleterMixin` — struct; pure virtual `completePrefix` and virtual destructor (defaulted).
- `nix::ReplPromptType` — `enum class`: `ReplPrompt`, `ContinuationPrompt`.
- `nix::ReplInteracter` — class; inner `using Guard = Finally<fun<void()>>`. Pure virtual `init`, pure virtual `getLine` (returns bool indicating EOF), virtual destructor.
- `nix::ReadlineLikeInteracter` — class; inherits `public virtual ReplInteracter`. Holds private `std::filesystem::path historyFile`. Inline constructor stores history path. Overrides `init`, `getLine`, virtual destructor.

### Functions
None at file scope (member declarations only).

### Type aliases
- `nix::ReplInteracter::Guard = Finally<fun<void()>>`

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/repl.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::AbstractNixRepl` — struct; abstract base for the Nix REPL implementation. Holds `ref<EvalState> state`, `Bindings * autoArgs`. Inline constructor takes `state`. Virtual destructor (inline, empty body). Inner aliases `AnnotatedValues = std::vector<std::pair<Value *, std::string>>` and `RunNix = void(const std::string &, OsStrings)`. Static `create` factory (returns `std::unique_ptr<AbstractNixRepl>`) and static `runSimple` helper (returns `ReplExitStatus`). Pure virtual `initEnv`, `mainLoop` (returns `ReplExitStatus`).

### Functions
None at file scope (member declarations only).

### Type aliases
- `nix::AbstractNixRepl::AnnotatedValues` (typedef)
- `nix::AbstractNixRepl::RunNix` (using alias)

### Macros / globals
None.

## File: src/libcmd/include/nix/cmd/unix-socket-server.hh

### Namespaces
- `nix::unix`

### Classes / structs / enums
- `nix::unix::PeerInfo` — struct; optional `pid`, `uid`, `gid` fields describing a Unix-socket peer.
- `nix::unix::ServeUnixSocketOptions` — struct; configures `serveUnixSocket`. Holds `socketPath` (`std::filesystem::path`), `socketMode = 0666` (`mode_t`), and (non-Windows only) `activationName = ""`, `auxiliaryFd = INVALID_DESCRIPTOR`, `onAuxiliaryFdPollin = nullptr` (`std::function<void()>`).
- `nix::unix::AbortServeSocket` — error type defined via `MakeError(AbortServeSocket, BaseError)`.

### Functions
- `nix::unix::getPeerInfo` — free function declaration; queries peer credentials from a socket fd `Descriptor remote`.
- `nix::unix::serveUnixSocket` — `[[noreturn]]` free function declaration; runs an accept loop, integrates with systemd socket activation.

### Type aliases
- `nix::unix::UnixSocketHandler = fun<void(AutoCloseFD socket, std::function<void()> closeListeners)>`

### Macros / globals
- `MakeError(AbortServeSocket, BaseError)` — macro generating the `AbortServeSocket` exception class.

## File: src/libcmd/installable-attr-path.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::InstallableAttrPath::InstallableAttrPath` — constructor; initialises `InstallableValue` base with `state`, sets `cmd`, allocates a root value via `allocRootValue(v)`, sets `attrPath`, moves `extendedOutputsSpec`.
- `nix::InstallableAttrPath::toValue` — override; resolves the attr path against `cmd.getAutoArgs(state)` via `findAlongAttrPath`, force-evaluates the result and returns `(vRes, pos)`.
- `nix::InstallableAttrPath::toDerivedPaths` — override; first tries `trySinglePathToDerivedPaths`; otherwise calls `getDerivations`, groups results by `drvPath`, computing `OutputsSpec` per the visit on `extendedOutputsSpec` (Default synthesises from `queryOutputs` falling back to `{"out"}`; Explicit returns spec verbatim); merges duplicates via `union_`. Wraps in `ExtraPathInfoValue` (FIXME notes backward-compat hack).
- `nix::InstallableAttrPath::parse` — static factory; treats `"."` as empty attr path; constructs an `InstallableAttrPath` (private ctor accessible via aggregate brace-init within the class).

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/installable-derived-path.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::InstallableDerivedPath::what` — override; returns `derivedPath.to_string(*store)`.
- `nix::InstallableDerivedPath::toDerivedPaths` — override; wraps `derivedPath` with empty `ExtraPathInfo` via `make_ref<ExtraPathInfo>()`.
- `nix::InstallableDerivedPath::getStorePath` — override; returns `derivedPath.getBaseStorePath()`.
- `nix::InstallableDerivedPath::parse` — static factory; visits `ExtendedOutputsSpec` to either follow symlinks (`store->followLinksToStorePath`) for `Default` (returning `DerivedPath::Opaque`) or build a typed `Built` path for `Explicit` (calling `drvRequireExperiment`).

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/installable-flake.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::InstallableFlake::getActualAttrPaths` — member function; if a single attr path begins with `.` strips it (mutates `attrPaths`), otherwise builds the `prefix + first` attempts plus the bare attr paths.
- `showAttrPaths` — file-static helper (no namespace prefix on definition); renders a list of attr paths for error messages with quoting and `or`/`,` separators using `enumerate`.
- `nix::InstallableFlake::InstallableFlake` — constructor; populates members; stores fragment as the only attr path when given (clearing `prefixes`); throws `UsageError` if `cmd` is non-null and `--arg`/`--argstr` were used (`getAutoArgs(*state)->size() > 0`).
- `nix::InstallableFlake::toDerivedPaths` — override; logs an `Activity`; via `getCursor`, either short-circuits to `trySinglePathToDerivedPaths` for non-derivations (else throws if not a derivation/path), or calls `forceDerivation`, computes `outputsToInstall` from `outputSpecified` (uses `outputName` when true) or `meta.outputsToInstall`, falls back to `{"out"}` when empty; pulls priority from `meta.priority`; wraps in `ExtraPathInfoFlake` carrying `originalRef`/`lockedRef`.
- `nix::InstallableFlake::toValue` — override; force-evaluates via `getCursor(state)->forceValue()`, returning `noPos`.
- `nix::InstallableFlake::getCursors` — override; opens an eval cache against the locked flake via `openEvalCache`; iterates `getActualAttrPaths`, calling `findAlongAttrPath` per candidate; throws with collected `Suggestions` if none resolve.
- `nix::InstallableFlake::getLockedFlake` — `const`; memoising lock; mutates a copy of `lockFlags` setting `applyNixConfig = true` (FIXME noted) and calls `lockFlake`.
- `nix::InstallableFlake::nixpkgsFlakeRef` — `const`; looks up `nixpkgs` input on the locked flake via `findInput({"nixpkgs"})`/`LockedNode`; falls back to `defaultNixpkgsFlakeRef`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/installable-value.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `nix::InstallableValue::getCursors` — virtual; constructs an in-memory `EvalCache` (with `std::nullopt` cache path) rooted at `toValue(state)` and returns its root.
- `nix::InstallableValue::getCursor` — virtual; returns first cursor via `at(0)` to avoid UB.
- `nonValueInstallable` — file-static helper returning a `UsageError` for installables that don't correspond to a Nix value.
- `nix::InstallableValue::require(Installable &)` — static; `dynamic_cast<InstallableValue *>` helper; throws if cast fails.
- `nix::InstallableValue::require(ref<Installable>)` — static; `dynamic_pointer_cast<InstallableValue>` helper; throws if cast fails.
- `nix::InstallableValue::trySinglePathToDerivedPaths` — protected; for `nPath` calls `fetchToStore` with `FetchMode::Copy`, returning a `DerivedPath::Opaque`; for `nString` calls `coerceToSingleDerivedPath` and on `Opaque` calls `state->ensureLazyPathCopied`, returning `DerivedPath::fromSingle`; otherwise `nullopt`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/installables.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `Aux` — local struct inside `Installable::build2`; holds `ref<ExtraPathInfo> info`, `ref<Installable> installable`.

### Functions
- `nix::completeFlakeInputAttrPath` — free function; iterates over flake inputs across given flake refs (`flake::getFlake` with `UseRegistries::All`), adding matching prefixes.
- `nix::MixFlakeOptions::MixFlakeOptions` — adds flake-related flags (under "Common flake-related options" category): `--recreate-lock-file` (deprecated warning), `--no-update-lock-file`, `--no-write-lock-file`, `--no-registries` (deprecated), `--commit-lock-file`, `--update-input` (deprecated), `--override-input`, `--reference-lock-file`, `--output-lock-file`, `--inputs-from`.
- `nix::SourceExprCommand::SourceExprCommand` — adds `--file`/`-f` (with `completePath`) and `--expr` flags under `installablesCategory`.
- `nix::MixReadOnlyOption::MixReadOnlyOption` — adds `--read-only` flag mutating `settings.readOnlyMode`.
- `nix::SourceExprCommand::getDefaultFlakeAttrPaths` — virtual; returns `{"packages.<system>.default", "defaultPackage.<system>"}`.
- `nix::SourceExprCommand::getDefaultFlakeAttrPathPrefixes` — virtual; returns `{"packages.<system>.", "legacyPackages.<system>."}`.
- `nix::SourceExprCommand::getCompleteInstallable` — returns a `CompleterClosure` that calls `completeInstallable`.
- `nix::SourceExprCommand::completeInstallable` — completion for `--file`-driven attr paths; otherwise delegates to `completeFlakeRefWithFragment`. Catches `EvalError` silently.
- `nix::completeFlakeRefWithFragment` — free function; flake#fragment completion driven by the eval cache; handles `.` prefix by clearing `attrPathPrefixes`; logs warnings on `Error`.
- `nix::completeFlakeRef` — free function; completes registry entries (handling `flake:` prefix specially) and directory paths via `Args::completeDir`; only enabled when the `flakes` experimental feature is on; adds `.` for empty prefix.
- `nix::Installable::toDerivedPath` — wraps `toDerivedPaths` requiring exactly one result; throws `Error` otherwise.
- `getDeriver` — file-static; queries `store->queryValidDerivers` and returns the first known deriver for a `StorePath`; throws if none (FIXME re using all derivers).
- `nix::SourceExprCommand::parseInstallables` — central installable-parsing routine; with `--file`/`--expr` (mutually exclusive) builds `InstallableAttrPath`s; `--file -` reads from stdin, `--file <path>` evals the file with base directory; otherwise tries `InstallableDerivedPath` (when prefix contains `/`, catching `BadStorePath`) then `InstallableFlake`; rethrows the stored exception on failure.
- `nix::SourceExprCommand::parseInstallable` — convenience wrapper for one installable; asserts size 1.
- `getBuiltPath` — file-static; recursively resolves a `SingleDerivedPath` to a `SingleBuiltPath`, calling `resolveDerivedPath` for `Built` variants.
- `nix::Installable::build` — static; returns flat vector of `BuiltPathWithResult` from `build2` results.
- `throwBuildErrors` — file-static; processes a vector of `KeyedBuildResult`, throwing the single failure or aggregating into a `build of %s failed` error.
- `nix::Installable::build2` — static; turns installables into `pathsToBuild` plus a backmap; for `Realise::Nothing|Derivation` calls `printMissing` and synthesises results; for `Realise::Outputs` calls `store->buildPathsWithResults` then `throwBuildErrors`; emits per-output `BuiltPath::Built` or `BuiltPath::Opaque` based on derived-path variant. Uses local `Aux`.
- `nix::Installable::toBuiltPaths` — static; either expands via `build` (Output) or via `toDerivations(store, installables, true)` (Derivation), emitting `BuiltPath::Opaque`. Sets `readOnlyMode` for `Realise::Nothing` in the Derivation branch.
- `nix::Installable::toStorePathSet` — static; flattens built paths to a `StorePathSet` via `outPaths`.
- `nix::Installable::toStorePaths` — static; flattens built paths to a `StorePaths` vector via `outPaths`.
- `nix::Installable::toStorePath` — static; require exactly one store path output; throws otherwise.
- `nix::Installable::toDerivations` — static; given installables and `useDeriver`, returns drv paths; for opaque paths uses the path itself if a derivation, else `getDeriver` if `useDeriver`, else throws; for built paths calls `resolveDerivedPath` on `drvPath`.
- `nix::RawInstallablesCommand::RawInstallablesCommand` — adds `--stdin` flag, then `expectArgs` for a positional `installables` collector with completer.
- `nix::RawInstallablesCommand::applyDefaultInstallables` — virtual; pushes `"."` if empty (FIXME re profile add).
- `nix::RawInstallablesCommand::getFlakeRefsForCompletion` — override; default-fills then parses each via `parseFlakeRefWithFragment` (with `expandTilde`).
- `nix::RawInstallablesCommand::run(ref<Store>)` — override; reads stdin word-by-word if `readFromStdIn` and `!isatty`, otherwise applies defaults; forwards to typed `run`.
- `nix::InstallableCommand::getFlakeRefsForCompletion` — override; parses the single `_installable` as a flake ref via `parseFlakeRefWithFragment`.
- `nix::InstallablesCommand::run(ref<Store>, std::vector<std::string> &&)` — override; parses raw installables via `parseInstallables` and forwards.
- `nix::InstallableCommand::InstallableCommand` — constructor; declares the optional positional argument `installable` with the completer (default `"."`).
- `nix::InstallableCommand::run(ref<Store>)` — override; parses single installable via `parseInstallable` and forwards.
- `nix::BuiltPathsCommand::applyDefaultInstallables` — override; pushes `"."` only when not `--all` and otherwise empty.
- `nix::toBuiltPaths(const std::vector<BuiltPathWithResult> &)` — free function; flattens `BuiltPathWithResult`s into a `BuiltPaths`.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/markdown.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `doRenderMarkdownToTerminal` — file-static (only when `HAVE_LOWDOWN`); configures `lowdown_opts`/`lowdown_opts_term` (cols based on terminal width clamped to ≥60), parses and renders Markdown to a string using `lowdown_doc_*`/`lowdown_term_*`. Uses `Finally` guards for free routines, `LOWDOWN_TERM_NOANSI` when not a TTY. Selects flag set via `HAVE_LOWDOWN_3` / `HAVE_LOWDOWN_1_4` / fallback.
- `nix::renderMarkdownToTerminal` — public free function; honours `_NIX_TEST_RAW_MARKDOWN=1` to bypass rendering. Falls back to identity when lowdown isn't available (alternate definition under `#else`).

### Type aliases
None.

### Macros / globals
- `HAVE_LOWDOWN`, `HAVE_LOWDOWN_1_4`, `HAVE_LOWDOWN_3` — feature-detection macros consumed via `#if`/`#elif`. Defined elsewhere via `cmd-config-private.hh`.

## File: src/libcmd/misc-store-flags.cc

### Namespaces
- `nix::flag`

### Classes / structs / enums
None.

### Functions
- `hashFormatCompleter` — file-static; completes hash-format names from `hashFormats`.
- `nix::flag::hashFormatWithDefault` — concrete impl; asserts `*hf == HashFormat::SRI` default.
- `nix::flag::hashFormatOpt` — concrete impl; for optional hash format.
- `hashAlgoCompleter` — file-static; completes hash-algorithm names from `hashAlgorithms`.
- `nix::flag::hashAlgo(std::string &&, HashAlgorithm *)` — concrete impl; description "blake3, md5, sha1, sha256, sha512".
- `nix::flag::hashAlgoOpt(std::string &&, std::optional<HashAlgorithm> *)` — concrete impl; notes SRI omission.
- `nix::flag::fileIngestionMethod` — concrete impl; documents `nar` and `flat` inputs.
- `nix::flag::contentAddressMethod` — concrete impl; documents `nar`, `flat`, `text` ingestion modes.

### Type aliases
None.

### Macros / globals
None.

## File: src/libcmd/network-proxy.cc

### Namespaces
- `nix`

### Classes / structs / enums
None.

### Functions
- `getAllVariables` — file-static; starts from `lowercaseVariables` and inserts an uppercase version of each via `std::transform` + `std::toupper`.
- `getExcludingNoProxyVariables` — file-static; subtracts `no_proxy`/`NO_PROXY` from `networkProxyVariables` via `std::set_difference`.
- `nix::haveNetworkProxyConnection` — checks whether any non-`no_proxy` var is set.

### Type aliases
None.

### Macros / globals
- `lowercaseVariables` — file-static `const StringSet`; the canonical lowercase proxy var names (`http_proxy`, `https_proxy`, `ftp_proxy`, `all_proxy`, `no_proxy`).
- `nix::networkProxyVariables` — `const StringSet`; defined here as the result of `getAllVariables()`, declared in header.
- `excludingNoProxyVariables` — file-static `const StringSet`; precomputed once.

## File: src/libcmd/repl-interacter.cc

### Namespaces
- `nix`
- anonymous namespace within `nix` (for `g_signal_received` and `sigintHandler`).

### Classes / structs / enums
None.

### Functions
- `sigintHandler` (anonymous namespace) — sets `g_signal_received` to the received signo.
- `completionCallback` — file-static (only when `!USE_READLINE`); editline completion bridge that returns the longest common prefix of completion candidates; allocates with `strdup`; catches all to return `nullptr`.
- `listPossibleCallback` — file-static (only when `!USE_READLINE`); editline list-possible bridge that allocates a C array of completions; bounds-checks against `INT_MAX/sizeof(char *)`.
- `nix::ReadlineLikeInteracter::init` — installs `rl_readline_name="nix-repl"`, ensures history file's parent dir exists, loads history (line-by-line via `FdSource::readLine` for editline to bypass its 256-byte SCREEN_INC limit; uses `read_history` for readline), saves/restores the `curRepl` pointer via `Guard`. Calls `rl_set_complete_func`/`rl_set_list_possib_func` for editline.
- `promptForType` — `static constexpr`; returns `"nix-repl> "` for `ReplPrompt` or `"        > "` for `ContinuationPrompt`.
- `nix::ReadlineLikeInteracter::getLine` — installs SIGINT handler (POSIX, sigprocmask unblock), runs `readline`, restores signals; returns true on signal (clearing input); falls through to `false` on null `s` (EOF). Optionally echoes input under `_NIX_TEST_REPL_ECHO=1` (editline-only path).
- `nix::ReadlineLikeInteracter::~ReadlineLikeInteracter` — writes history to disk via `write_history`.

### Type aliases
None.

### Macros / globals
- `g_signal_received` — `volatile sig_atomic_t` in anonymous namespace.
- `curRepl` — file-static `detail::ReplCompleterMixin *`; current REPL pointer (commented "ugly").
- `USE_READLINE` — feature macro selecting between readline and editline (toggled via `cmd-config-private.hh`).

## File: src/libcmd/repl.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `nix::ProcessLineResult` — `enum class`: `Quit`, `Continue`, `PromptAgain`. Returned by `NixRepl::processLine`.
- `nix::NixRepl` — struct; inherits `AbstractNixRepl`, `detail::ReplCompleterMixin`, `gc`. Concrete REPL implementation. Holds `size_t debugTraceIndex`, `std::list<std::filesystem::path> loadedFiles`, `Strings loadedFlakes`, `fun<AnnotatedValues()> getValues`, `std::shared_ptr<StaticEnv> staticEnv`, `std::optional<Value> lastLoaded`, `Env * env`, `int displ`, `StringSet varNames`, `RunNix * runNixPtr`, `std::unique_ptr<ReplInteracter> interacter`. Static `const int envSize = 32768`. Constructor takes `lookupPath`, `state`, `getValues`, `runNix`. Default destructor. Methods `runNix`, `mainLoop` (override returns `ReplExitStatus`), `initEnv` (override), `completePrefix` (override), `getDerivationPath`, `processLine` (returns `ProcessLineResult`), `loadFile`, `loadFlake`, `loadFiles`, `loadFlakes`, `reloadFilesAndFlakes`, `showLastLoaded`, `addAttrsToScope`, `addVarToScope`, `parseString`, `parseReplBindings`, `evalString`, `loadDebugTraceEnv`, `printValue` (defined inline).
- `nix::IncompleteReplExpr` — error type defined via `MakeError(IncompleteReplExpr, Error)`; thrown when REPL input is incomplete (e.g. unclosed expression) so the main loop prompts for continuation.

### Functions
- `nix::removeWhitespace` — free helper; chomps then strips leading whitespace from a string.
- `showDebugTrace` — file-static (returns `std::ostream &`); prints a `DebugTrace` with optional code lines via `printCodeLines`.
- `isIncompleteInput` — file-static; identifies a `ParseError` whose message contains "unexpected end of file" so the REPL can prompt for continuation.
- `nix::NixRepl::NixRepl` — constructor; initialises base, debug-trace index, callbacks, `staticEnv` from `state->staticBaseEnv`, runNix pointer, and a `ReadlineLikeInteracter` rooted at `getDataDir() / "repl-history"`.
- `nix::NixRepl::mainLoop` — override; prints banner on first invocation (with " debugger" if `state->debugRepl`), calls `loadFiles`, registers itself with the interacter, then loops on `interacter->getLine`; switches on `processLine` result for `Quit`/`Continue`/`PromptAgain`; catches `IncompleteReplExpr` to continue, logs other `Error`/`Interrupted`. Returns `ReplExitStatus::QuitAll` on Ctrl-D or `:q`, `ReplExitStatus::Continue` on `:c`/`:s`.
- `nix::NixRepl::completePrefix` — override; does path completion when `cur` contains `/`, var-name completion when no `.`, otherwise evaluates the prefix expression and lists its attrs (catching `ParseError`/`EvalError`/`BadURL`/`FileNotFound`). Temporarily disables debugger.
- `nix::NixRepl::getDerivationPath` — gets a derivation's `drvPath` via `getDerivation`, validates it exists in the store; throws if not a derivation.
- `nix::NixRepl::loadDebugTraceEnv` — calls `initEnv`, then maps the static env via `mapStaticEnvBindings` and adds each var to the scope.
- `nix::NixRepl::processLine` — central dispatcher; commands: `:?`/`:help`, `:bt`/`:backtrace` (debugRepl), `:env` (debugRepl), `:st [idx]` (debugRepl), `:s`/`:step` (debugRepl), `:c`/`:continue` (debugRepl), `:a`/`:add`, `:l`/`:load` (resets file cache), `:lf`/`:load-flake`, `:ll`/`:last-loaded`, `:r`/`:reload` (resets file cache), `:e`/`:edit` (uses `editorFor` + `runProgram2`), `:t` (showType), `:u` (drv-as-shell), `:b`/`:bl`/`:i`/`:sh`/`:log` (build, with `:bl` adding `repl-result-<output>` symlinks; `:log` uses `fetchBuildLog` and `RunPager`), `:p`/`:print`, `:q`/`:quit`, `:doc` (renders Markdown synopsis + doc, with fallback for ExprSelect), `:te`/`:trace-enable`. Falls back to `parseReplBindings` (binding form) then `evalString` (expression form, printing with `maxDepth=1`).
- `nix::NixRepl::loadFile` — evals a file, auto-applies `autoArgs`, calls `addAttrsToScope`; updates `loadedFiles` only on success.
- `nix::NixRepl::loadFlake` — parses flake ref, refuses unlocked refs under `pureEval`, calls `flake::callFlake`/`lockFlake` and `addAttrsToScope`; updates `loadedFlakes` only on success.
- `nix::NixRepl::initEnv` — allocates env (size `envSize=32768`), points `up` at `state->baseEnv`, clears static env vars and `varNames`, repopulates `varNames` from `state->staticBaseEnv`.
- `nix::NixRepl::showLastLoaded` — prints last-loaded attribute names through a `RunPager`; ignores `EPIPE`.
- `nix::NixRepl::reloadFilesAndFlakes` — calls `initEnv`, `loadFiles`, `loadFlakes`.
- `nix::NixRepl::loadFiles` — re-loads each previously-loaded file (preserving failed entries); also loads installables via `getValues()`.
- `nix::NixRepl::loadFlakes` — re-loads each previously-loaded flake (preserving failed entries).
- `nix::NixRepl::addAttrsToScope` — forces attrs, asserts envSize budget, copies bindings into `staticEnv`/`env`, sorts/dedupes, prints first 20 names with "... and N more" suffix; updates `lastLoaded`.
- `nix::NixRepl::addVarToScope` — replaces any existing entry in `staticEnv`, appends new value to `env->values`.
- `nix::NixRepl::parseString` — parses an expression; rethrows `IncompleteReplExpr` if the parse error is "unexpected end of file".
- `nix::NixRepl::parseReplBindings` — tries to parse as bindings (handling `inherit foo` shorthand by retrying with `;` appended); returns `nullptr` on non-binding parse error; rethrows `IncompleteReplExpr` for incomplete input.
- `nix::NixRepl::evalString` — parses, evaluates, force-evaluates the result.
- `nix::NixRepl::runNix` — calls `runNixPtr` if non-null; else throws an `Error` referencing Nix 2.25 release notes.
- `nix::AbstractNixRepl::create` — factory; constructs a `NixRepl` via `std::make_unique`.
- `nix::AbstractNixRepl::runSimple` — convenience entry that builds a minimal `NixRepl` with empty `getValues`, calls `initEnv`, adds each `extraEnv` entry to scope, and runs the main loop. Returns `ReplExitStatus`.

### Type aliases
None new at file scope.

### Macros / globals
- `isFirstRepl` — file-static `bool` (initial `true`); controls whether the REPL prints its banner.
- `MakeError(IncompleteReplExpr, Error)` — macro generating the `IncompleteReplExpr` exception type.

## File: src/libcmd/unix/unix-socket-server.cc

### Namespaces
- `nix::unix`

### Classes / structs / enums
None.

### Functions
- `nix::unix::getPeerInfo` — implementation; uses `SO_PEERCRED` (Linux/OpenBSD via `sockpeercred`/`ucred`) to fill all of `pid`/`uid`/`gid`, or `LOCAL_PEERCRED` (Apple/FreeBSD via `xucred`) to fill only `uid`.
- `nix::unix::serveUnixSocket` — `[[noreturn]]` accept loop. Detects systemd activation via `LISTEN_FDS`/`LISTEN_PID`/`LISTEN_FDNAMES` (asserting `LISTEN_PID == getpid()`), filters by `activationName`; otherwise `createDirs` and `createUnixDomainSocket`. Polls listening sockets plus optional `auxiliaryFd` (with `onAuxiliaryFdPollin` callback), accepts connections via `accept`, calls handler with a `closeListeners` callback. Catches `AbortServeSocket` to bail out, logs other `Error`s with `while processing connection:` prefix.

### Type aliases
None.

### Macros / globals
- `SD_LISTEN_FDS_START` — `static constexpr int` = 3 (systemd convention).
- `SOL_LOCAL` — `#define SOL_LOCAL 0` fallback (when not provided by the system) inside the `LOCAL_PEERCRED` branch of `getPeerInfo`.

---

## Cross-file observations

### Installable hierarchy
The `Installable` base in `installables.hh` is overridden by:
- `InstallableValue` (in `installable-value.hh`) — abstract intermediate; adds `EvalState`-aware machinery (`state` member, `toValue`, virtual `getCursors`/`getCursor`, `toApp`, protected `trySinglePathToDerivedPaths`, static `require`).
- `InstallableAttrPath` (in `installable-attr-path.hh`) extends `InstallableValue`. Its `toDerivedPaths` walks `getDerivations` results and de-duplicates by `drvPath` via `byDrvPath` map merging via `OutputsSpec::union_`.
- `InstallableFlake` (in `installable-flake.hh`) extends `InstallableValue`. Its `toDerivedPaths` walks an eval cache cursor and emits a single `DerivedPath::Built`, attaching `ExtraPathInfoFlake` with `originalRef`/`lockedRef`.
- `InstallableDerivedPath` (in `installable-derived-path.hh`) extends `Installable` directly (not `InstallableValue`) — this is the only subclass that does not require evaluation; carries a `ref<Store>` and uses it to render `what`.

Override map (each `Installable*` member function and where it is overridden):
- `what`: pure in `Installable`; concrete in `InstallableAttrPath` (inline returning `attrPath`), `InstallableDerivedPath` (returns `derivedPath.to_string(*store)`), `InstallableFlake` (inline returning `flakeRef.to_string() + "#" + *attrPaths.begin()`).
- `toDerivedPaths`: pure in `Installable`; concrete in `InstallableAttrPath`, `InstallableDerivedPath`, `InstallableFlake`.
- `toDerivedPath`: concrete in `Installable` (single-result wrapper).
- `getStorePath`: virtual in `Installable` (default `{}`); overridden in `InstallableDerivedPath` (returns `derivedPath.getBaseStorePath()`).
- `toValue`: pure in `InstallableValue`; concrete in `InstallableAttrPath`, `InstallableFlake`.
- `getCursors`: virtual in `InstallableValue` (in-memory `EvalCache`); overridden in `InstallableFlake` (real `EvalCache` against locked flake).
- `getCursor`: virtual in `InstallableValue` (calls `getCursors().at(0)`); not overridden.

Duplication / refactoring opportunities visible within this shard:
- `toDerivedPaths` in both `InstallableAttrPath` and `InstallableFlake` perform the same `ExtendedOutputsSpec` visit (`Default` synthesises `outputsToInstall` then defaults to `{"out"}`; `Explicit` returns the spec verbatim) when materialising `OutputsSpec`. Extracting an `outputsSpecFromExtended(Default callback, Explicit)` helper would deduplicate the two visit blocks.
- The opening invocation of `trySinglePathToDerivedPaths` is identical in `InstallableAttrPath::toDerivedPaths` and `InstallableFlake::toDerivedPaths` (single-path short-circuit before treating the value as a derivation). Today this lives twice; a templated method on `InstallableValue` could host it.
- `getCursors` in `InstallableValue` (single in-memory cache) and `InstallableFlake` (multiple attr-path attempts on a real cache) share the suggestion-collection pattern with `findAlongAttrPath`. Only minor scope, but could be unified if `InstallableValue::getCursors` were factored to take a candidate-list source.
- `static InstallableX parse(...)` factories — `InstallableAttrPath`, `InstallableDerivedPath`, `InstallableFlake` all expose a static `parse` plus a private/public constructor. The signatures vary, so the pattern is loose.

### Command hierarchy
The `Command` base is the central abstract class declared elsewhere (in libmain). Within this shard, the inheritance graph rooted at `Command` adds:
- `NixMultiCommand` inherits `MultiCommand` and `virtual Command`.
- `StoreConfigCommand` inherits `virtual Command`.
- `StoreCommand` inherits `virtual StoreConfigCommand`.
- `CopyCommand` inherits `virtual StoreCommand`.
- `EvalCommand` inherits `virtual StoreCommand` and (non-virtually) `MixEvalArgs`.
- `MixFlakeOptions` inherits `virtual Args` and (non-virtually) `EvalCommand`.
- `SourceExprCommand` inherits `virtual Args` and (non-virtually) `MixFlakeOptions`.
- `RawInstallablesCommand` inherits `virtual Args` and (non-virtually) `SourceExprCommand`.
- `InstallablesCommand` inherits (publicly, non-virtually) `RawInstallablesCommand`.
- `InstallableCommand` inherits `virtual Args` and (non-virtually) `SourceExprCommand`.
- `InstallableValueCommand` (in `command-installable-value.hh`) inherits `InstallableCommand`.
- `BuiltPathsCommand` inherits (non-virtually) `InstallablesCommand` and `virtual MixOperateOnOptions`.
- `StorePathsCommand` inherits (publicly) `BuiltPathsCommand`.
- `StorePathCommand` inherits (publicly) `StorePathsCommand`.

Mixins (each is a thin `virtual Args` mixin whose constructor calls `addFlag({...})` for one or more options, except where noted):
- `MixEvalArgs` (in `common-eval-args.hh`) inherits `virtual Args`, `virtual MixRepair`. Adds `--arg`, `--argstr`, `--arg-from-file`, `--arg-from-stdin`, `-I`/`--include`, `--impure`, `--override-flake`, `--eval-store`.
- `MixFlakeOptions` (in `command.hh`) — extends `virtual Args`, `EvalCommand`. Adds nine flake-related flags.
- `MixReadOnlyOption` (in `command.hh`) — `virtual Args`. Adds `--read-only`.
- `MixOperateOnOptions` (in `command.hh`) — `virtual Args`. Adds `--derivation`.
- `MixProfile` (in `command.hh`) — `virtual StoreCommand` (not `virtual Args`). Adds `--profile`.
- `MixDefaultProfile` — extends `MixProfile`. Sets default profile in constructor (no flag added).
- `MixEnvironment` (in `command.hh`) — `virtual Args`. Adds `-i`/`--ignore-env`, `-k`/`--keep-env-var`, `-u`/`--unset-env-var`, `-s`/`--set-env-var`.
- `MixNoCheckSigs` (in `command.hh`) — `virtual Args`, inline ctor. Adds `--no-check-sigs`.
- `MixOutLinkBase` (in `command.hh`) — `virtual Args`. Holds `outLink` (no flag added by base).
- `MixOutLinkByDefault` — extends `MixOutLinkBase`, `virtual Args`. Adds `-o`/`--out-link`, `--no-link`.

The `run` chain `StoreConfigCommand::run() → run(StoreConfig)` then `StoreCommand::run(StoreConfig) → run(Store)` then `BuiltPathsCommand → StorePathsCommand → StorePathCommand` is fully sequential; each step does nothing more than narrow the type. The chain is fragile to GCC's `-Woverloaded-virtual` warning (suppressed by a pragma in `command.hh`). It could be flattened by templated CRTP rather than virtual `run` overloads.

The repetitive use of `Args::Flag` initialisers (longName, shortName, description, category, handler, completer) is the dominant boilerplate in this shard. A small flag-builder DSL or constexpr table could reduce this dramatically.

`BuiltPathsCommand::run` and `Installable::toBuiltPaths` both branch on the `OperateOn::Output`/`Derivation` distinction and emit `BuiltPath::Opaque` for derivations. The closure-recursion logic in `BuiltPathsCommand::run` is independent of `toBuiltPaths` but conceptually adjacent.

### `BuiltPath` / `SingleBuiltPath` symmetry
`built-path.hh`/`built-path.cc` define near-mirrored types: `SingleBuiltPath`+`SingleBuiltPathBuilt` vs `BuiltPath`+`BuiltPathBuilt`. The implementations of `outPath`/`outPaths`, `discardOutputPath`, and `toJSON` follow identical visit-dispatch templates but are duplicated verbatim because of the single-vs-multi output schism. The `==`/`<=>` story is also asymmetric — `Single*` gets full ordering (defaulted on the variant struct, custom via `GENERATE_CMP_EXT` for `SingleBuiltPathBuilt`), `BuiltPath`/`BuiltPathBuilt` only equality (defaulted on the variant struct, `GENERATE_EQUAL` for `BuiltPathBuilt`), due to libc++ 16 (Darwin) missing `std::map::operator<=>`. A future libc++ bump would let these collapse to defaulted operators.

`BuiltPath::Opaque` and `SingleBuiltPath::Opaque` are both type-aliased to `DerivedPathOpaque` — only the `Built` differs in cardinality.

### Registry pattern
`RegisterCommand` (in `command.hh`/`command.cc`) and `RegisterLegacyCommand` (in `legacy.hh`/`command.cc`) share an identical singleton-map-with-static-constructor registration idiom. Their `Commands` types differ in:
- key type: `std::vector<std::string>` for `RegisterCommand`, `std::string` for `RegisterLegacyCommand`;
- value signature: `fun<ref<Command>()>` vs `fun<void(int, char **)>` (i.e. `MainFunction`);
- insertion strategy: `RegisterCommand` uses `emplace` (first-write-wins); `RegisterLegacyCommand` uses `insert_or_assign` (last-write-wins).
Neither subclasses a shared base. A templated `Registry<Key, Value>` would eliminate ~10 lines of duplication.

### Settings registration
`common-eval-args.cc` registers four global settings (`fetchSettings`, `evalSettings`, `flakeSettings`, `compatibilitySettings`) with four anonymous `GlobalConfig::Register` instances. These globals come from different libraries (fetchers/, expr/, flake/, cmd/) but share the lifecycle. The repetition is small but the static-initialisation order is implicit and could be fragile. Note that `evalSettings` is constructed with a non-trivial lambda registering a `flake:` lookup-path resolver inline (so its construction depends on `fetchSettings` already being live).

### Markdown / readline feature gating
Both `markdown.cc` and `repl-interacter.cc` include `cmd-config-private.hh` (a generated header outside this shard) and switch implementations on macros (`HAVE_LOWDOWN*`, `USE_READLINE`). The patterns are independent but show the same "config-private build-time toggle" style.

### `parseInstallables` flow
`SourceExprCommand::parseInstallables` is the sole construction point for all three concrete `Installable` subclasses:
- with `--file` / `--expr`: only `InstallableAttrPath`;
- otherwise: tries `InstallableDerivedPath` first (only when prefix contains `/`, catching `BadStorePath` to fall through), then `InstallableFlake` as the catch-all. Other exceptions are stashed and rethrown if both attempts fail.

### REPL command-dispatch table
`NixRepl::processLine` is implemented as a long chained `if`/`else if` switch on the `:command` token. The dispatcher mixes evaluation commands (`:t`, `:p`, `:doc`, default expression evaluation) with side-effecting build commands (`:b`/`:bl`/`:i`/`:sh`/`:log` collapsed into one branch via `command ==` comparisons), debug-only commands (`:bt`, `:env`, `:st`, `:s`, `:c`) gated on `state->debugRepl`, and lifecycle commands (`:q`, `:r`, `:l`, `:lf`, `:e`). The `IncompleteReplExpr` exception type plus `isIncompleteInput` check enables the main loop to switch from `ReplPrompt` to `ContinuationPrompt` when the user submits an unfinished expression.
