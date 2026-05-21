# Inventory — Shard 17: nix CLI (modern subcommands part 1)

## File: src/nix/main.cc

### Namespaces
- `nix` — top-level namespace for the CLI entry point.

### Classes / structs / enums
- `NixArgs` (struct) — root argument parser; `virtual MultiCommand, virtual MixCommonArgs, virtual RootArgs`. Holds `useNet`, `refresh`, `helpRequested`, `showVersion` flags; sets up command categories (`catHelp`, `Command::catDefault`, `catSecondary`, `catUtility`, `catNixInstallation`), top-level flags (`--help`, `--print-build-logs`/`-L`, `--version`, `--offline` (with deprecated alias `--no-net`), `--refresh`), and an `aliases` map mapping deprecated/short command names to canonical paths.
- `CmdHelp` (struct, derives `Command`) — `nix help` subcommand. Constructor registers a `subcommand` positional; `run` walks parents to top-level then calls `showHelp(subcommand, getNixArgs(*this))`.
- `CmdHelpStores` (struct, derives `Command`) — `nix help-stores` subcommand; `run` calls `showHelp({"help-stores"}, ...)`.

### Functions
- `chrootHelper(int argc, char ** argv)` — `extern` declaration (defined in `run.cc`); single-threaded helper to set up bind-mount/chroot for diverted stores. Linux-only (guarded by `#ifndef _WIN32`).
- `haveInternet()` (static) — probes `getifaddrs` for non-loopback/non-link-local interfaces and falls back to `haveNetworkProxyConnection`; on Windows returns `true` unconditionally.
- `showHelp(std::vector<std::string> subcommand, NixArgs & toplevel)` (static) — resolves single-word subcommand aliases, sets `restrictEval`/`pureEval`, evaluates the embedded `generate-manpage.nix.gen.hh` glue (plus `utils.nix`, `generate-settings.nix`, `generate-store-info.nix` files added to `corepkgsFS`), passes `dumpCli()` JSON, looks up the rendered `<name>.md` attribute, and pipes it through `RunPager` + `renderMarkdownToTerminal`.
- `getNixArgs(Command & cmd)` (static) — `dynamic_cast`s `cmd.getRoot()` to `NixArgs &`.
- `mainWrapped(int argc, char ** argv)` — main body: stores `savedArgv`, calls `registerCrashHandler()`, dispatches to `chrootHelper` if argv[0] matches, sets the build hook to `getNixBin({})` + `__build-remote`, runs `initNix`, `initGC`, `flakeSettings.configureEvalSettings`, on Linux as root sets up a private mount namespace, normalises `programName`, handles `__build-remote` shim, dispatches to a `RegisterLegacyCommand` if matched, sets `pureEval = true`, picks log format/verbosity, builds `NixArgs args`, handles `__dump-cli`/`__dump-language`/`__dump-xp-features`, prints completions, parses argv (with shebang support if program ends with "nix"), applies the JSON logger, handles `--help`/completions/`--version`, requires the command's experimental feature, applies `--offline` overrides (substituters, tarballTtl, file-transfer tries, connectTimeout, narInfoDiskCache ttlMeta), applies `--refresh`, applies `forceImpureByDefault`, then runs `args.command->second->run()` (re-throwing `eval_cache::CachedEvalError` after `e.force()`).
- `int main(int argc, char ** argv)` — sets `nixVersion = NIX_CLI_VERSION` and calls `handleExceptions(argv[0], [&]() { mainWrapped(argc, argv); })`.
- Member functions on `NixArgs`: `description()`, `doc()` (includes `nix.md`), `pluginsInited()` (re-fetches commands so plugins can register new ones), `dumpCli()` (returns JSON with `args`/`stores`/`fetchers` sub-trees).
- Member functions on `CmdHelp` / `CmdHelpStores`: `description()`, `doc()`, `category()` (both return `catHelp`), `run()`.

### Command registrations
- `registerCommand<CmdHelp>("help")` — `rCmdHelp`.
- `registerCommand<CmdHelpStores>("help-stores")` — `rCmdHelpStores`.

### Type aliases
- None.

### Macros / globals
- `extern std::string chrootHelperName` — declared in this file (defined in `run.cc`); name of the `__run_in_chroot` helper.
- `std::string programPath` — set from `argv[0]` in `mainWrapped`, used elsewhere in the CLI.

## File: src/nix/crash-handler.cc

### Namespaces
- `nix` — top-level.
- Anonymous nested namespace inside `nix`.

### Classes / structs / enums
- None.

### Functions
- `logFatal(std::string const & s)` (anonymous namespace) — writes a line to stderr via `writeToStderr` and (non-Windows) syslog with `LOG_CRIT`.
- `onTerminate()` (anonymous namespace) — `std::set_terminate` callback: logs the "Nix crashed" banner, rethrows current exception to print its demangled type+message, prints `boost::stacktrace::stacktrace()`, then `std::abort`s.
- `registerCrashHandler()` — installs `onTerminate` via `std::set_terminate`. Comment notes this is not used for signals (boost stacktrace is not async-signal-safe; ASLR also makes addr2line pointless).

### Command registrations
- None.

### Type aliases
- None.

### Macros / globals
- `BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED` — defined on Apple/FreeBSD before including `boost/stacktrace.hpp`.

## File: src/nix/crash-handler.hh

### Namespaces
- `nix`.

### Functions
- `void registerCrashHandler()` — declaration.

### Macros / globals
- None.

## File: src/nix/man-pages.cc

### Namespaces
- `nix`.

### Functions
- `getNixManDir()` — returns `canonPath(std::filesystem::path{NIX_MAN_DIR})`.
- `showManPage(const std::string & name)` — `restoreProcessContext`, sets `MANPATH` to `<nixManDir>:`, `execlp`s `man <name>`; if `errno == ENOENT` throws an `Error` instructing the user to install `man`, otherwise `SysError`.

### Command registrations
- None.

### Macros / globals
- None (uses `NIX_MAN_DIR` macro from `cli-config-private.hh`).

## File: src/nix/man-pages.hh

### Namespaces
- `nix`.

### Functions
- `std::filesystem::path getNixManDir()` — declaration.
- `void showManPage(const std::string & name)` — declaration.

## File: src/nix/self-exe.cc

### Namespaces
- `nix`.

### Functions
- `getNixBin(std::optional<std::string_view> binaryNameOpt)` — local lambda `getBinaryName` defaulting to `"nix"`. Resolution order: `NIX_BIN_DIR` env (joined with the binary name); `getSelfExe()` heuristic (with the requested binary name swapped in if provided); compile-time `NIX_BIN_DIR` fallback path; finally returns just the bare binary name (relying on `PATH`).

### Command registrations
- None.

### Type aliases
- None.

### Macros / globals
- None (uses `NIX_BIN_DIR` macro from `cli-config-private.hh`).

## File: src/nix/self-exe.hh

### Namespaces
- `nix`.

### Functions
- `std::filesystem::path getNixBin(std::optional<std::string_view> binary_name = {})` — declaration.

## File: src/nix/run.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdRun` (struct, derives `InstallableValueCommand, MixEnvironment`) — `nix run` subcommand. Stores extra `args` positional (with `completePath`); overrides `getDefaultFlakeAttrPaths` to push `apps.<system>.default` + `defaultApp.<system>` ahead of base prefixes; `getDefaultFlakeAttrPathPrefixes` pushes `apps.<system>.`; `run` evaluates the installable, resolves the app, clears `state->evalCaches`, calls `setEnviron()`, then `execProgramInStore(store, UseLookupPath::DontUse, ...)`.

### Functions
- `toEnvp(StringMap env)` — converts an env map to `Strings` of `KEY=VALUE` lines.
- `execProgramInStore(ref<Store>, UseLookupPath, const std::string & program, const Strings & args, std::optional<std::string_view> system, std::optional<StringMap> env)` — central program-launching primitive. Stops the logger; builds `envp` from `env` (or inherits `environ`); `restoreProcessContext`; if the store is not a `LocalFSStore` throws; if `storeDir != getRealStoreDir()` re-execs `getSelfExe()` with `chrootHelperName` and arguments; on Linux sets personality if `system` provided; then either `execvp` (with manual `environ = envp` fixup, no `execvpe` on macOS) or `execve`.
- `chrootHelper(int argc, char ** argv)` — Linux-only helper invoked when this binary's argv[0] equals `chrootHelperName`. Parses `storeDir`, `realStoreDir`, `system`, `cmd`, remaining `args`. `unshare(CLONE_NEWUSER | CLONE_NEWNS)` (falls back to plain `CLONE_NEWNS`). If `storeDir` does not exist, builds a temp chroot with bind-mounted `realStoreDir` and bind-mounts root entries; otherwise tries an `overlay` mount, falling back to a bind mount. Writes `setgroups=deny`, `uid_map`, `gid_map`. Sets personality if `system != ""`. `execvp`s the requested command. Throws `Error` on non-Linux platforms.
- Member functions on `CmdRun`: `description()`, `doc()`, `getDefaultFlakeAttrPaths()`, `getDefaultFlakeAttrPathPrefixes()`, `run(ref<Store>, ref<InstallableValue>)`.

### Command registrations
- `registerCommand<CmdRun>("run")` — `rCmdRun`.

### Type aliases
- None.

### Macros / globals
- `std::string chrootHelperName = "__run_in_chroot"` — sentinel argv[0] for the helper invocation.
- `extern char ** environ __attribute__((weak))` — forward decl of libc `environ` (weak) used to inherit current env.

## File: src/nix/run.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum struct UseLookupPath { Use, DontUse }` — selects whether `execProgramInStore` should use `execvp` (PATH lookup) vs `execve`.

### Functions
- `void execProgramInStore(ref<Store>, UseLookupPath, const std::string & program, const Strings & args, std::optional<std::string_view> system = std::nullopt, std::optional<StringMap> env = std::nullopt)` — declaration with default args.

### Macros / globals
- None.

## File: src/nix/build.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdBuild` (struct, derives `InstallablesCommand, MixOutLinkByDefault, MixDryRun, MixJSON, MixProfile`) — `nix build` subcommand. Holds `printOutputPaths` (default false), `buildMode` (default `bmNormal`). Adds `--print-out-paths` and `--rebuild` (sets `buildMode = bmCheck`). `run` either prints missing dry-run paths (and JSON if `--json`), or builds via `Installable::build`, optionally JSON-dumps `builtPathsWithResultToJSON`, calls `createOutLinksMaybe`, optionally prints output paths, and `updateProfile`s with the built paths.

### Functions
- `toJSON(Store & store, const SingleDerivedPath::Opaque & o)` (static) — returns the printed store path string.
- `toJSON(Store & store, const SingleDerivedPath & sdp)` (static, forward decl + impl) — `std::visit` over the variant.
- `toJSON(Store & store, const DerivedPath & dp)` (static, forward decl + impl) — `std::visit` over the variant.
- `toJSON(Store & store, const SingleDerivedPath::Built & sdpb)` (static) — emits drvPath/output/outputPath JSON, looking up the output via `queryPartialDerivationOutputMap(resolveDerivedPath(...))`.
- `toJSON(Store & store, const DerivedPath::Built & dpb)` (static) — emits drvPath plus per-requested-output JSON.
- `derivedPathsToJSON(const DerivedPaths & paths, Store & store)` (static) — array of `toJSON(...)` results.
- `builtPathsWithResultToJSON(const std::vector<BuiltPathWithResult> &, const Store &)` (static) — adds `startTime`/`stopTime`/`cpuUser`/`cpuSystem` (cpu values divided by 1e6) to each path's JSON.
- Member functions on `CmdBuild`: `description()`, `doc()`, `run(ref<Store>, Installables &&)`.

### Command registrations
- `registerCommand<CmdBuild>("build")` — `rCmdBuild`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/eval.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdEval` (struct, derives `MixJSON, InstallableValueCommand, MixReadOnlyOption`) — `nix eval` subcommand. Flags: `--raw` (sets `raw`), `--apply <expr>` (sets `apply`), `--write-to <path>` (sets `writeTo`). `run` rejects `--raw` + `--json`, evaluates the installable, optionally applies a function, then either recursively writes a string/attrs-of-strings tree to disk (using a `this`-deducing recursive lambda), or writes raw to stdout, or prints JSON via `printValueAsJSON`, or prints via `ValuePrinter`. Calls `state->ensureLazyPathsCopied(context)`.

### Functions
- Member functions on `CmdEval`: `description()`, `doc()`, `category()` (returns `catSecondary`), `run(ref<Store>, ref<InstallableValue>)`.

### Command registrations
- `registerCommand<CmdEval>("eval")` — `rCmdEval`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/repl.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdRepl` (struct, derives `RawInstallablesCommand`) — `nix repl` subcommand. Constructor sets `evalSettings.pureEval = false`. Holds `files` member. Overrides `experimentalFeature()` to return `nullopt` (stable), `getDefaultFlakeAttrPaths()` to return `{""}`, `forceImpureByDefault()` to return `true`. `applyDefaultInstallables` injects `"."` if no installables but `file` or `expr` set. `run` builds an `AbstractNixRepl` via `AbstractNixRepl::create(lookupPath, state, getValues, runNix)`, calls `initEnv` and `mainLoop`.

### Functions
- `runNix(const std::string & program, OsStrings args)` — re-invokes a Nix sibling binary (`getNixBin(program)`) with `NIX_CONFIG = globalConfig.toKeyValue()` in the env and `isInteractive = true`.
- Member functions on `CmdRepl`: `experimentalFeature()`, `getDefaultFlakeAttrPaths()`, `forceImpureByDefault()`, `description()`, `doc()`, `applyDefaultInstallables(...)`, `run(ref<Store>, std::vector<std::string> && rawInstallables)`.

### Command registrations
- `registerCommand<CmdRepl>("repl")` — `rCmdRepl`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/search.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdSearch` (struct, derives `InstallableValueCommand, MixJSON`) — `nix search` subcommand. Stores positional `res` (regexes) and `excludeRes` (`--exclude/-e`). `getDefaultFlakeAttrPaths` returns `packages.<system>` + `legacyPackages.<system>`. `run` sets `readOnlyMode = true` and disables IFD, builds `boost::regex` lists, walks the eval-cache attr tree (deriving recursion rules from `legacyPackages`/`packages`/`recurseForDerivations`), matching each `attrPath`/`name`/`description` against the regexes, emitting highlighted output (terminal) or JSON.

### Functions
- `wrap(std::string prefix, std::string s)` (non-static) — wraps `s` between `prefix` and `ANSI_NORMAL`.
- Member functions on `CmdSearch`: `description()`, `doc()`, `getDefaultFlakeAttrPaths()`, `run(ref<Store>, ref<InstallableValue>)`.

### Command registrations
- `registerCommand<CmdSearch>("search")` — `rCmdSearch`.

### Type aliases
- `using json = nlohmann::json` (file-scope, before namespace).

### Macros / globals
- None.

## File: src/nix/develop.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `DevelopSettings` (struct, derives `Config`) — exposes `bashPrompt`, `bashPromptPrefix`, `bashPromptSuffix` `Setting<std::string>` members used to customise `nix develop` shells.
- `BuildEnvironment` (struct) — represents the environment dumped by `get-env.sh`. Contents:
  - Nested `String` (struct) — `{exported, value}` pair plus `operator==`.
  - `Array` (using-alias) — `std::vector<std::string>`.
  - `Associative` (using-alias) — `StringMap`.
  - `Value` (using-alias) — `std::variant<String, Array, Associative>`.
  - Members: `vars` (map), `bashFunctions`, optional `structuredAttrs` (sh+json pair).
  - Methods: static `fromJSON`, static `parseJSON`, `toJSON` (asserts round-trip), `providesStructuredAttrs`, `getAttrsJSON`, `getAttrsSH`, `toBash`, static `getString`, static `getAssociative`, static `getStrings`, `operator==`, `getSystem` (defaults to `settings.thisSystem`).
- `Common` (struct, derives `InstallableCommand, MixProfile`) — base class for `develop` / `print-dev-env`. Holds default `ignoreVars` set, `redirects` vector, the `--redirect` flag (label "installable", "outputs-dir"); methods `makeRcScript`, `fixupStructuredAttrs`, `getDefaultFlakeAttrPaths` (devShells.<system>.default + devShell.<system>), `getDefaultFlakeAttrPathPrefixes` (devShells.<system>.), `getShellOutPath`, `getBuildEnvironment`.
- `CmdDevelop` (struct, derives `Common, MixEnvironment`) — `nix develop` subcommand. Adds `--command/-c`, `--phase`, plus phase shortcut flags (`--unpack`, `--configure`, `--build`, `--check`, `--install`, `--installcheck` setting `phase` to the corresponding name; `installcheck` maps to `installCheck`); builds the dev shell, looks up `bashInteractive` from the flake's nixpkgs ref (or `defaultNixpkgsFlakeRef()`), sets `NIX_GCROOT`, `setEnviron`, optionally `chdir`s into the flake source for `--phase`, clears `state->evalCaches`, and `execProgramInStore` with the looked-up bash. Throws `UnimplementedError` on Windows.
- `CmdPrintDevEnv` (struct, derives `Common, MixJSON`) — `nix print-dev-env` subcommand. Prints the rc script (or JSON via `printJSON(buildEnvironment.toJSON())`) reproducing the build environment.

### Functions
- `getDerivationEnvironment(ref<Store>, ref<Store>, const StorePath &)` (static) — clones a derivation, asserts builder is bash, `addToStoreFromDump`s `getEnvSh` content, points the builder at it, removes derivation output checks, replaces InputAddressed/CAFixed outputs with Deferred, rewrites the name to "<name>-env", `fillInOutputPaths`, `writeDerivation`, `buildPaths`, then reads the first non-empty output path's contents.
- Member functions on the structs above (constructors, `description`, `doc`, `category`, `run`, etc., as listed inline). `Common::makeRcScript` builds an rc script that saves PATH/XDG_DATA_DIRS, calls `BuildEnvironment::toBash`, restores them, sets `NIX_BUILD_TOP` to a fresh tmp dir, evaluates `shellHook`, performs output-path rewrites (incl. `--redirect` matches), and (for structuredAttrs) writes `.attrs.sh`/`.attrs.json` files via `fixupStructuredAttrs`.

### Command registrations
- `registerCommand<CmdPrintDevEnv>("print-dev-env")` — `rCmdPrintDevEnv`.
- `registerCommand<CmdDevelop>("develop")` — `rCmdDevelop`.

### Type aliases
- `BuildEnvironment::Array` (= `std::vector<std::string>`).
- `BuildEnvironment::Associative` (= `StringMap`).
- `BuildEnvironment::Value` (= `std::variant<String, Array, Associative>`).

### Macros / globals
- `static DevelopSettings developSettings` — global instance.
- `static GlobalConfig::Register rDevelopSettings(&developSettings)` — registration with global config.
- `const static std::string getEnvSh` — embeds `get-env.sh.gen.hh` (file-scope, in namespace).

## File: src/nix/bundle.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdBundle` (struct, derives `InstallableValueCommand`) — `nix bundle` subcommand. Stores `bundler` (default `"github:NixOS/bundlers"`) and `outLink`. Flags: `--bundler/-B <flake-url>` (with flake-ref completer) and `--out-link/-o <path>`. Constructs an `InstallableFlake` for the chosen bundler with default attr paths `bundlers.<system>.default` + `defaultBundler.<system>` and prefix `bundlers.<system>.`, applies it to the target installable, validates `drvPath`/`outPath` attrs, builds, defaults `outLink` to the bundler result's `name` attr, and `addPermRoot`s.

### Functions
- Member functions on `CmdBundle`: `description()`, `doc()`, `category()` (returns `catSecondary`), `getDefaultFlakeAttrPaths()`, `getDefaultFlakeAttrPathPrefixes()`, `run(ref<Store>, ref<InstallableValue>)`.

### Command registrations
- `registerCommand<CmdBundle>("bundle")` — `r2`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/app.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- None defined here (uses `App`, `UnresolvedApp`, etc., declared in libcmd headers).

### Functions
- `resolveRewrites(Store &, const std::vector<BuiltPathWithResult> & dependencies)` — computes downstream-placeholder→output-path rewrites for CA-derivation app contexts. Returns empty when `Xp::CaDerivations` is disabled.
- `resolveString(Store &, const std::string & toResolve, const std::vector<BuiltPathWithResult> &)` — applies the rewrites from `resolveRewrites` via `rewriteStrings`.
- `InstallableValue::toApp(EvalState & state)` — out-of-class definition. Inspects the installable's attr (`type` ∈ {`app`, `derivation`} based on attrPath prefix); for "app" pulls `program` (with context) and converts the context elements to `DerivedPath`s; for "derivation" derives the program path as `outPath/bin/<mainProgram or pname or DrvName(name).name>`. Calls `state.ensureLazyPathsCopied(context)` for the app branch.
- `UnresolvedApp::build(ref<Store> evalStore, ref<Store> store)` — wraps each context element in `InstallableDerivedPath` and calls `Installable::build(... Realise::Outputs ...)`.
- `UnresolvedApp::resolve(ref<Store> evalStore, ref<Store> store)` — builds the context, rewrites the program path, asserts `store->isInStore(...)`, returns a resolved `App`.

### Command registrations
- None.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/env.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdEnv` (struct, derives `NixMultiCommand`) — `nix env` parent subcommand (utility category) backed by `RegisterCommand::getCommandsFor({"env"})` via the constructor.
- `CmdShell` (struct, derives `InstallablesCommand, MixEnvironment`) — `nix env shell` subcommand. Default `command` is `[$SHELL or "bash"]`, `--command/-c` overrides it. `run` resolves output paths, walks them via a queue tracking `done` (using `boost::unordered_flat_set`), resolves `bin` symlinks via `state->storeFS`, follows `nix-support/propagated-user-env-packages`, prepends to `PATH`, clears `state->evalCaches`, and calls `execProgramInStore(store, UseLookupPath::Use, *command.begin(), args)`.

### Functions
- Member functions on `CmdEnv`: `description()`, `category()` (returns `catUtility`).
- Member functions on `CmdShell`: `description()`, `doc()`, `run(ref<Store>, Installables &&)`.

### Command registrations
- `registerCommand<CmdEnv>("env")` — `rCmdEnv`.
- `registerCommand2<CmdShell>({"env", "shell"})` — `rCmdShell`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/flake.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdFlakeUpdate` (forward-declared at file scope, then full definition; derives `FlakeCommand`). Constructor clears `expectedArgs`, adds `--flake <flake-url>` flag and an `inputs` positional that updates `lockFlags.inputUpdates`, removes flags `no-update-lock-file` and `no-write-lock-file`. `run` clears `tarballTtl`, sets `recreateLockFile` (when no inputs supplied) / `writeLockFile = true` / `applyNixConfig = true`, then `lockFlake()`.
- `CmdFlakeLock` (struct, derives `FlakeCommand`) — `nix flake lock`. Constructor removes `no-write-lock-file`. `run` sets `writeLockFile=true`, `failOnUnlocked=true`, `applyNixConfig=true`, then `lockFlake()`.
- `CmdFlakeMetadata` (struct, derives `FlakeCommand, MixJSON`) — `nix flake metadata`; prints (or JSON-dumps) the locked flake's metadata and recursively renders the input tree via a `this`-deducing recursive lambda using `treeLast`/`treeConn`/`treeNull`/`treeLine` glyphs.
- `CmdFlakeInfo` (struct, derives `CmdFlakeMetadata`) — overrides `run` to warn (deprecated alias) and delegate to `CmdFlakeMetadata::run`.
- `CmdFlakeCheck` (struct, derives `FlakeCommand`) — `nix flake check`; flags `--no-build`, `--all-systems`. Walks the flake outputs (`checks`, `formatter`, `packages`/`devShells`, `apps`, `defaultPackage`/`devShell`, `defaultApp`, `legacyPackages`, `overlay`, `overlays`, `nixosModule`, `nixosModules`, `nixosConfigurations`, `hydraJobs`, `defaultTemplate`, `templates`, `defaultBundler`, `bundlers`, plus a community-attr allowlist and a warning for unknown outputs), then optionally builds matching local-system check derivations using `queryMissing` to skip already-substitutable paths.
- `CmdFlakeInitCommon` (struct, derives `virtual Args, EvalCommand`) — base for `init`/`new`. Holds `templateUrl = "templates"`, `destDir`, `lockFlags{.writeLockFile = false}`; constructor adds `--template/-t` with completion. `run` resolves the template, recursively copies its tree (using a `this`-deducing recursive lambda) refusing to overwrite differing files; if `.git` exists runs `git -C ... add --intent-to-add --force --`; renders `welcomeText`; throws on conflict.
- `CmdFlakeInit` (struct, derives `CmdFlakeInitCommon`) — sets `destDir = "."` in ctor.
- `CmdFlakeNew` (struct, derives `CmdFlakeInitCommon`) — adds `dest-dir` positional (with `completePath`).
- `CmdFlakeClone` (struct, derives `FlakeCommand`) — `--dest <path>` (`-f`); calls `getFlakeRef().resolve(...).input.clone(...)`.
- `CmdFlakeArchive` (struct, derives `FlakeCommand, MixJSON, MixDryRun, MixNoCheckSigs`) — `--to <store-uri>`; copies flake + inputs via `lockedRef.input.fetchToStore` (or `computeStorePath` when dryRun), JSON-dumps the input tree, then `copyPaths` to `dstUri` if not dry-run.
- `CmdFlakeShow` (struct, derives `FlakeCommand, MixJSON`) — `--legacy`, `--all-systems`; renders a tree-formatted view of flake outputs (or JSON) using two recursive lambdas (`hasContent`, `visit`) plus `recurse` and `showDerivation` lambdas inside.
- `CmdFlakePrefetch` (struct, derives `FlakeCommand, MixJSON`) — `--out-link/-o <path>`; downloads the flake source tree via `lazyFetch` + `fetchToStore`, prints store path + SRI hash + original/locked attrs.
- `CmdFlake` (struct, derives `NixMultiCommand`) — top-level `nix flake` parent (constructor passes `RegisterCommand::getCommandsFor({"flake"})`); overrides `run` to `experimentalFeatureSettings.require(Xp::Flakes)` then dispatch.

### Functions
- `FlakeCommand::FlakeCommand()` (out-of-class) — registers `flake-url` optional positional with flake-ref completion.
- `FlakeCommand::getFlakeRef()` — `parseFlakeRef(fetchSettings, flakeUrl, std::filesystem::current_path().string())`.
- `FlakeCommand::lockFlake()` — `flake::lockFlake(flakeSettings, *getEvalState(), getFlakeRef(), lockFlags)`.
- `FlakeCommand::getFlakeRefsForCompletion()` — like `getFlakeRef` but with `expandTilde` applied first; returns single-element vector.
- `enumerateOutputs(EvalState &, Value & vFlake, callback)` (static) — invokes callback per flake output, ensuring `hydraJobs` is processed first (so IFD can be disabled for it and re-enabled for others).
- Numerous lambdas in `CmdFlakeCheck::run` (`reportError`, `resolve`, `argHasName`, `checkSystemName`, `checkSystemType`, `checkDerivation`, `checkApp`, `checkOverlay`, `checkModule`, `checkHydraJobs`, `checkNixOSConfiguration`, `checkTemplate`, `checkBundler`).
- `CmdFlakeShow::run` lambdas: `hasContent`, `visit`, plus inner `recurse`/`showDerivation`.

### Command registrations
- `registerCommand<CmdFlake>("flake")` — `rCmdFlake`.
- `registerCommand2<CmdFlakeArchive>({"flake", "archive"})` — `rCmdFlakeArchive`.
- `registerCommand2<CmdFlakeCheck>({"flake", "check"})` — `rCmdFlakeCheck`.
- `registerCommand2<CmdFlakeClone>({"flake", "clone"})` — `rCmdFlakeClone`.
- `registerCommand2<CmdFlakeInfo>({"flake", "info"})` — `rCmdFlakeInfo`.
- `registerCommand2<CmdFlakeInit>({"flake", "init"})` — `rCmdFlakeInit`.
- `registerCommand2<CmdFlakeLock>({"flake", "lock"})` — `rCmdFlakeLock`.
- `registerCommand2<CmdFlakeMetadata>({"flake", "metadata"})` — `rCmdFlakeMetadata`.
- `registerCommand2<CmdFlakeNew>({"flake", "new"})` — `rCmdFlakeNew`.
- `registerCommand2<CmdFlakePrefetch>({"flake", "prefetch"})` — `rCmdFlakePrefetch`.
- `registerCommand2<CmdFlakeShow>({"flake", "show"})` — `rCmdFlakeShow`.
- `registerCommand2<CmdFlakeUpdate>({"flake", "update"})` — `rCmdFlakeUpdate`.

### Type aliases
- None.

### Macros / globals
- `static Strings defaultTemplateAttrPathsPrefixes{"templates."}` — used by template completion.
- `static Strings defaultTemplateAttrPaths = {"templates.default", "defaultTemplate"}` — used by template completion.

## File: src/nix/flake-command.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `FlakeCommand` (class, derives `virtual Args, public MixFlakeOptions`) — common base for flake subcommands. Protected `flakeUrl = "."`; declares `FlakeCommand()`, `getFlakeRef()`, `lockFlake()`, override `getFlakeRefsForCompletion()`.

### Functions
- See `FlakeCommand` member declarations above.

### Macros / globals
- None.

## File: src/nix/flake-prefetch-inputs.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdFlakePrefetchInputs` (struct, derives `FlakeCommand`) — `nix flake prefetch-inputs`; concurrent `ThreadPool` (sized by `fileTransferSettings.httpConnections`) walk over the locked-flake graph that fetches each `LockedNode` via `fetchToStore`. Tracks `nrFailed` (atomic); throws `Exit(nrFailed ? 1 : 0)`.
  - Local `State` struct (only `done` set of `const Node *`) used under `Sync<State>`.

### Functions
- `CmdFlakePrefetchInputs::description`, `doc`, `run(nix::ref<nix::Store>)` (member functions); local `visit` as a `this`-deducing recursive lambda.

### Command registrations
- `registerCommand2<CmdFlakePrefetchInputs>({"flake", "prefetch-inputs"})` — `rCmdFlakePrefetchInputs`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/registry.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `RegistryCommand` (class, derives `virtual Args`) — registers `--registry <registry>` flag; private `registry_path` and lazily-initialised `registry` shared pointer. Public methods `getRegistry()` (returns user registry by default, or `getCustomRegistry` for a path) and `getRegistryPath()` (returns user registry path string by default).
- `CmdRegistryList` (struct, derives `StoreCommand`) — `nix registry list`; iterates `getRegistries(...)` (Flag/User/System/Global) and prints "<type> <from> <to>".
- `CmdRegistryAdd` (struct, derives `MixEvalArgs, Command, RegistryCommand`) — `nix registry add`. Positionals `from-url` and `to-url`; replaces matching entry in the user registry, copying `subdir` into `extraAttrs["dir"]`, then writes back.
- `CmdRegistryRemove` (struct, derives `RegistryCommand, Command`) — `nix registry remove`. Positional `url`.
- `CmdRegistryPin` (struct, derives `RegistryCommand, EvalCommand`) — `nix registry pin`. Positionals `url` and optional `locked` (defaults to `url`); resolves and locks the flake, warns if not locked, writes the pin (with `dir` extra attr if `subdir != ""`).
- `CmdRegistryResolve` (struct, derives `StoreCommand`) — `nix registry resolve`. Positional list `flake-refs`; prints each resolved URL.
- `CmdRegistry` (struct, derives `NixMultiCommand`) — top-level `nix registry`. Constructor lists subcommands inline via lambda factories (`list`/`add`/`remove`/`pin`/`resolve`); `category()` returns `catSecondary`.

### Functions
- `RegistryCommand::getRegistry()` — load (cached) registry pointer.
- `RegistryCommand::getRegistryPath()` — resolves the on-disk path to write to.
- `description`, `doc`, `run(...)` overrides on each command struct.

### Command registrations
- `registerCommand<CmdRegistry>("registry")` — `rCmdRegistry`.
- The five subcommands (`list`/`add`/`remove`/`pin`/`resolve`) are registered through the inline `NixMultiCommand` table rather than via separate `registerCommand2` calls.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/profile.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `ProfileElementSource` (struct) — `originalRef`, `lockedRef`, `attrPath`, `outputs`. Comparison via `operator<` (TODO `<=>` blocked by libc++ 16); `to_string`.
- `ProfileElement` (struct) — `storePaths`, optional `source`, `active = true`, `priority = defaultPriority`. Methods: `identifier`, `toInstallables`, `versions`, `updateStorePaths`.
- `ProfileManifest` (struct) — `using ProfileElementName = std::string`; `elements` map. Methods: default ctor; ctor from `EvalState`+profile path (parses `manifest.json` versions 1/2/3 or legacy `manifest.nix` via `queryInstalled`); `addElement(nameCandidate, element)` (deduplicates by appending `-N`); `addElement(element)`; `toJSON(Store &) const` (always emits `version: 3`); `build(ref<Store>)`; static `printDiff`.
- `Matcher` (struct) — abstract base; virtual destructor; virtual `getTitle`, `matches`.
- `RegexMatcher` (struct, `final`, derives `Matcher`).
- `StorePathMatcher` (struct, `final`, derives `Matcher`).
- `NameMatcher` (struct, `final`, derives `Matcher`).
- `AllMatcher` (struct, `final`, derives `Matcher`).
- `MixProfileElementMatchers` (class, `virtual Args, virtual StoreCommand, public virtual MixDefaultProfile`) — adds `--all`, `--regex <pattern>`, plus `elements` positional; resolves each positional as a stored-int (rejected), `StorePath`, or name match; `getMatchingElementNames(manifest)`; private `completeProfileElements`.
- `CmdProfileAdd` (struct, derives `InstallablesCommand, MixDefaultProfile`) — `nix profile add`; `--priority`. Builds installables, populates `ProfileElement` entries (priority from flag → `ExtraPathInfoValue` → `defaultPriority`), refuses duplicate adds when source/priority match, writes the new profile generation. Includes long error message for `BuildEnvFileConflictError` showing how to resolve via `--priority`.
- `CmdProfileRemove` (struct, derives `virtual EvalCommand, MixProfileElementMatchers`) — `nix profile remove`.
- `CmdProfileUpgrade` (struct, derives `virtual SourceExprCommand, MixProfileElementMatchers, MixDryRun`) — `nix profile upgrade`; clears `tarballTtl`, refreshes flake inputs and re-evaluates each matched element; skips locked sources or non-flake entries.
- `CmdProfileList` (struct, derives `virtual EvalCommand, virtual StoreCommand, MixDefaultProfile, MixJSON`) — `nix profile list`.
- `CmdProfileDiffClosures` (struct, derives `virtual StoreCommand, MixDefaultProfile`) — `nix profile diff-closures`.
- `CmdProfileHistory` (struct, derives `virtual StoreCommand, EvalCommand, MixDefaultProfile`) — `nix profile history`.
- `CmdProfileRollback` (struct, derives `virtual StoreCommand, MixDefaultProfile, MixDryRun`) — `nix profile rollback`; `--to <version>`.
- `CmdProfileWipeHistory` (struct, derives `virtual StoreCommand, MixDefaultProfile, MixDryRun`) — `nix profile wipe-history`; `--older-than <age>` (`Nd` format).
- `CmdProfile` (struct, derives `NixMultiCommand`) — top-level `nix profile`; constructor lists 8 subcommands via lambda factories and adds `aliases = {{"install", {AliasStatus::Deprecated, {"add"}}}}`.

### Functions
- `getNameFromElement(const ProfileElement &)` — chooses a profile-entry name by URL or identifier.
- `builtPathsPerInstallable(...)` (static) — groups built paths by source installable.
- Many member functions across all the structs above (constructors, `description`, `doc`, `run`, etc.).

### Command registrations
- `registerCommand<CmdProfile>("profile")` — `rCmdProfile`.
- The eight subcommands (`add`, `remove`, `upgrade`, `list`, `diff-closures`, `history`, `rollback`, `wipe-history`) are registered through the inline `NixMultiCommand` table.

### Type aliases
- `ProfileManifest::ProfileElementName = std::string`.

### Macros / globals
- `const int defaultPriority = 5` — default priority for new profile elements.
- `AllMatcher all` — single global instance reused across `--all` flag invocations (with custom no-op deleter when wrapped in a `ref`).

## File: src/nix/path-info.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdPathInfo` (struct, derives `StorePathsCommand, MixJSON`) — `nix path-info`. Flags: `--size/-s`, `--closure-size/-S`, `--human-readable/-h`, `--sigs`, `--json-format <version>` (parses to `PathInfoJsonFormat`). Prints sizes/sigs per path or emits JSON; warns when `--json` given without `--json-format`.

### Functions
- `getStoreObjectsTotalSize(Store &, const StorePathSet & closure)` (static) — sum of `narSize` for the closure.
- `pathInfoToJSON(Store &, const StorePathSet &, bool showClosureSize, PathInfoJsonFormat format)` (static) — produces V1 or V2 JSON path-info output; for `NarInfo`-typed entries also includes `closureDownloadSize`.
- Member functions on `CmdPathInfo`: `description`, `doc`, `category` (returns `catSecondary`), `printSize` (renders size with optional human-friendly format), `run(ref<Store>, StorePaths &&)`.

### Command registrations
- `registerCommand<CmdPathInfo>("path-info")` — `rCmdPathInfo`.

### Type aliases
- File-scope `using nlohmann::json` (inside namespace `nix`).

### Macros / globals
- None.

## File: src/nix/path-from-hash-part.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdPathFromHashPart` (struct, derives `StoreCommand`) — `nix store path-from-hash-part`. Expects positional `hash-part`; prints the matching store path or throws.

### Command registrations
- `registerCommand2<CmdPathFromHashPart>({"store", "path-from-hash-part"})` — `rCmdPathFromHashPart`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/prefetch.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdStorePrefetchFile` (struct, derives `StoreCommand, MixJSON`) — `nix store prefetch-file`. Flags: `--name`, `--expected-hash`, `--hash-type` (via `flag::hashAlgo`), `--executable`, `--unpack`; positional `url`. Prints either notice + store path or JSON.
- `MyArgs` (local struct inside `main_nix_prefetch_url`) — derives `LegacyArgs, MixEvalArgs`; inherits `LegacyArgs` constructors.

### Functions
- `resolveMirrorUrl(EvalState &, const std::string & url)` — resolves `mirror://NAME/file` URLs by importing `<nixpkgs/pkgs/build-support/fetchurl/mirrors.nix>` and picking the first mirror.
- `prefetchFile(ref<Store>, const VerbatimURL &, std::optional<std::string> maybeName, HashAlgorithm, std::optional<Hash> expectedHash, bool unpack, bool executable)` — picks `NixArchive` or `Flat` ContentAddressMethod; resolves a fallback name from URL; if `expectedHash` matches an existing valid path, returns it; otherwise downloads (optionally executable bit), optionally `unpackTarfile`s, calls `addToStoreSlow`, and returns `{StorePath, Hash}`.
- `main_nix_prefetch_url(int argc, char ** argv)` (static) — implementation of the legacy `nix-prefetch-url` command. Hand-rolls argv parsing through `MyArgs` (a local struct deriving `LegacyArgs, MixEvalArgs`), handles `--help`, `--version`, `--type`, `--print-path`, `--attr/-A`, `--unpack`, `--executable`, `--name`. Optionally evaluates a Nix expression + attrPath to extract `urls[0]` (and `outputHashMode`/`name`), then calls `prefetchFile`.
- Member functions on `CmdStorePrefetchFile`: `description`, `doc`, `run(ref<Store>)`.

### Command registrations
- `RegisterLegacyCommand r_nix_prefetch_url("nix-prefetch-url", main_nix_prefetch_url)` — legacy command.
- `registerCommand2<CmdStorePrefetchFile>({"store", "prefetch-file"})` — `rCmdStorePrefetchFile`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/realisation.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdRealisation` (struct, derives `NixMultiCommand`) — top-level `nix realisation` parent (utility category) backed by `RegisterCommand::getCommandsFor({"realisation"})`.
- `CmdRealisationInfo` (struct, derives `BuiltPathsCommand, MixJSON`) — `nix realisation info`. `category()` returns `catSecondary`. `run` requires `Xp::CaDerivations`, walks built paths via `toRealisedPaths`, prints realisation IDs + outPaths or JSON (with `opaquePath` fallback for opaque paths).

### Command registrations
- `registerCommand<CmdRealisation>("realisation")` — `rCmdRealisation`.
- `registerCommand2<CmdRealisationInfo>({"realisation", "info"})` — `rCmdRealisationInfo`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/derivation.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdDerivation` (struct, derives `NixMultiCommand`) — top-level `nix derivation` parent (utility category) backed by `RegisterCommand::getCommandsFor({"derivation"})`.

### Command registrations
- `registerCommand<CmdDerivation>("derivation")` — `rCmdDerivation`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/derivation-add.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdAddDerivation` (struct, derives `MixDryRun, StoreCommand`) — `nix derivation add`. `category()` returns `catUtility`. `run` reads JSON from stdin via `drainFD(STDIN_FILENO)`, parses+validates a `Derivation` via `Derivation::parseJsonAndValidate`, optionally writes it (or computes its store path via `computeStorePath` if `dryRun || readOnlyMode`), and prints the resulting drv path.

### Command registrations
- `registerCommand2<CmdAddDerivation>({"derivation", "add"})` — `rCmdAddDerivation`.

### Type aliases
- File-scope `using json = nlohmann::json` (before namespace).

### Macros / globals
- None.

## File: src/nix/derivation-show.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdShowDerivation` (struct, derives `InstallablesCommand, MixPrintJSON`) — `nix derivation show`. `--recursive/-r` flag (closure expansion); `category()` returns `catUtility`. `run` writes a JSON `{ "version": expectedJsonVersionDerivation, "derivations": { drvPath: drvJson } }` via `printJSON`.

### Command registrations
- `registerCommand2<CmdShowDerivation>({"derivation", "show"})` — `rCmdShowDerivation`.

### Type aliases
- File-scope `using json = nlohmann::json` (before namespace).

### Macros / globals
- None.

## File: src/nix/edit.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdEdit` (struct, derives `InstallableValueCommand`) — `nix edit`. `category()` returns `catSecondary`. `run` resolves package position via `findPackageFilename` (mapping `NoPositionInfo` to a friendly `Error`), then `runProgram2` with the front of `editorFor(file, line, /*readOnly=*/true)` interactively (`isInteractive = true`, `lookupPath = true`).

### Command registrations
- `registerCommand<CmdEdit>("edit")` — `rCmdEdit`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/config.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdConfig` (struct, derives `NixMultiCommand`) — top-level `nix config` parent backed by `RegisterCommand::getCommandsFor({"config"})`; `category()` returns `catUtility`.
- `CmdConfigShow` (struct, derives `Command, MixJSON`) — `nix config show [name]`; rejects `--json` together with a name; prints either a single setting value or the whole config (key/value or JSON via `globalConfig.toKeyValue()` / `globalConfig.toJSON()`).

### Command registrations
- `registerCommand<CmdConfig>("config")` — `rCmdConfig`.
- `registerCommand2<CmdConfigShow>({"config", "show"})` — `rShowConfig`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/config-check.cc

### Namespaces
- `nix`.
- Anonymous nested namespace inside `nix`.

### Classes / structs / enums
- `CmdConfigCheck` (struct, derives `StoreCommand`) — `nix config check`. Holds `success = true`. Aggregates checks `checkNixInPath`, `checkProfileRoots`, `checkStoreProtocol(store->getProtocol())`, `checkTrustedUser`. Throws `Exit(2)` on failure. Marked stable (`experimentalFeature()` returns `nullopt`); `category()` returns `catNixInstallation`.

### Functions
- `formatProtocol(unsigned int proto)` (anonymous namespace) — pretty-prints `WorkerProto::Version::Number::fromWire(proto)` as `major.minor` or "unknown".
- `checkPass(std::string_view msg)` (anonymous namespace) — `notice` green PASS line, returns `true`.
- `checkFail(std::string_view msg)` (anonymous namespace) — `notice` red FAIL line, returns `false`.
- `checkInfo(std::string_view msg)` (anonymous namespace) — `notice` blue INFO line.
- Member functions on `CmdConfigCheck`: `experimentalFeature`, `description`, `category`, `run`, `checkNixInPath`, `checkProfileRoots`, `checkStoreProtocol`, `checkTrustedUser`.

### Command registrations
- `registerCommand2<CmdConfigCheck>({"config", "check"})` — `rCmdConfigCheck`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/diff-closures.cc

### Namespaces
- `nix`.
- Anonymous nested namespace inside `nix`.

### Classes / structs / enums
- `Info` (struct, anonymous namespace) — small helper holding `outputName`.
- `CmdDiffClosures` (struct, derives `SourceExprCommand, MixOperateOnOptions`) — `nix store diff-closures`. Two positional installables `_before`, `_after`; resolves their store paths and prints the closure diff.

### Functions
- `getClosureInfo(ref<Store>, const StorePath & toplevel)` — computes `GroupedPaths` (name → version → path → Info) by walking the closure and stripping ambiguous output suffixes via a hard-coded regex `(.*)-([a-z]+|lib32|lib64)`.
- `showVersions(const StringSet & versions)` — formats versions, mapping empty to "ε" and empty set to "∅".
- `printClosureDiff(ref<Store>, const StorePath & beforePath, const StorePath & afterPath, std::string_view indent)` — colored diff (added/removed versions and net size delta in bytes; size delta only shown when `|delta| >= 8 KiB`).
- `CmdDiffClosures::description`, `doc`, `run`.

### Command registrations
- `registerCommand2<CmdDiffClosures>({"store", "diff-closures"})` — `rCmdDiffClosures`.

### Type aliases
- `typedef std::map<std::string, std::map<std::string, std::map<StorePath, Info>>> GroupedPaths` — name → version → path → Info.

### Macros / globals
- None.

## File: src/nix/why-depends.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdWhyDepends` (struct, derives `SourceExprCommand, MixOperateOnOptions`) — `nix why-depends`. Two positional installables `_package`, `_dependency`; flags `--all/-a`, `--precise`. `category()` returns `catSecondary`. `run` defines two local helper types:
  - `Node` (struct, local in `run`) — Dijkstra search node holding `path`, `refs`, `rrefs`, `dist` (default `inf`), `prev`, `queued`, `visited`.
  - `BailOut` (empty struct, local in `run`) — used as an exception to stop early traversal.

### Functions
- `hilite(const std::string & s, size_t pos, size_t len, const std::string & colour = ANSI_RED)` (static) — wraps a substring with ANSI colour markers.
- `filterPrintable(const std::string & s)` (static) — replaces non-printable bytes with `.`.
- Member functions on `CmdWhyDepends`: `description`, `doc`, `category`, `run(ref<Store>)`. The `run` method also defines a recursive `printNode` lambda (typed via `fun<void(Node &, const std::string &, const std::string &)>` to allow self-recursion). `printNode` uses `scanForReferencesDeep` (when `--precise`) to colour-highlight in-store references.

### Command registrations
- `registerCommand<CmdWhyDepends>("why-depends")` — `rCmdWhyDepends`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/log.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdLog` (struct, derives `InstallableCommand`) — `nix log`. `category()` returns `catSecondary`. `run` sets `readOnlyMode = true`, resolves `installable->toDerivedPath()`, picks the drv path (or wraps an opaque path), `resolveDerivedPath`s it, runs `RunPager` and `fetchBuildLog`, and writes the log via `writeFull(getStandardOutput(), log)`.

### Command registrations
- `registerCommand<CmdLog>("log")` — `rCmdLog`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/formatter.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdFormatter` (struct, derives `NixMultiCommand`) — `nix formatter` parent backed by `RegisterCommand::getCommandsFor({"formatter"})`; `category()` returns `catSecondary`.
- `MixFormatter` (struct, derives `SourceExprCommand`) — common base supplying `getDefaultFlakeAttrPaths()` (returns `formatter.<system>` only) and empty `getDefaultFlakeAttrPathPrefixes()`.
- `CmdFormatterRun` (struct, derives `MixFormatter, MixJSON`) — `nix formatter run`. Stores positional `args`. `category()` returns `catSecondary`. Resolves `parseInstallable(store, ".")` cast to `InstallableFlake`, calls `toApp(...).resolve(...)`, sets `PRJ_ROOT` to the flake source path, clears `evalState->evalCaches`, and `execProgramInStore(store, UseLookupPath::DontUse, app.program, args, std::nullopt, env)`.
- `CmdFormatterBuild` (struct, derives `MixFormatter, MixOutLinkByDefault`) — `nix formatter build`. `category()` returns `catSecondary`. Resolves the formatter app, `unresolvedApp.build(...)`, calls `createOutLinksMaybe`, prints `app.program`.
- `CmdFmt` (struct, derives `CmdFormatterRun`) — top-level `nix fmt` shorthand; `run` delegates to `CmdFormatterRun::run`.

### Command registrations
- `registerCommand<CmdFormatter>("formatter")` — `rCmdFormatter`.
- `registerCommand2<CmdFormatterRun>({"formatter", "run"})` — `rFormatterRun`.
- `registerCommand2<CmdFormatterBuild>({"formatter", "build"})` — `rFormatterBuild`.
- `registerCommand<CmdFmt>("fmt")` — `rFmt`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/nix/upgrade-nix.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `UpgradeSettings` (struct, derives `Config`) — single `Setting<std::string> storePathUrl` defaulting to nixpkgs `nix-fallback-paths.nix` (key `upgrade-nix-store-path-url`).
- `CmdUpgradeNix` (struct, derives `MixDryRun, StoreCommand`) — `nix upgrade-nix`. Flags `--profile/-p`, `--nix-store-paths-url`. Implements `getProfileDir`, `getLatestNix`, `run`. Marked stable (`experimentalFeature()` returns `nullopt`); `category()` returns `catNixInstallation`. Verifies new Nix and runs `nix-env -i --no-sandbox` against the profile.

### Functions
- `hasProfilesComponent(const std::filesystem::path &)` (static) — uses `std::ranges::contains(path, OS_STR("profiles"))` to check for a `profiles` segment.
- Member functions on `CmdUpgradeNix`: `experimentalFeature`, `description`, `doc`, `category`, `run(ref<Store>)`, `getProfileDir(ref<Store>)`, `getLatestNix(ref<Store>)`.

### Command registrations
- `registerCommand<CmdUpgradeNix>("upgrade-nix")` — `rCmdUpgradeNix`.

### Type aliases
- None.

### Macros / globals
- `UpgradeSettings upgradeSettings` — global instance.
- `static GlobalConfig::Register rUpgradeSettings(&upgradeSettings)` — registers it.

## File: src/nix/hash.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdHashBase` (struct, derives `Command`) — base for `nix hash path/file` and the legacy `nix-hash`. Holds `mode` (`FileIngestionMethod`, set by ctor argument), `hashFormat = HashFormat::SRI`, `truncate = false`, `hashAlgo = HashAlgorithm::SHA256`, `paths`, optional `modulus`. Adds `--sri`, `--base64`, `--base32`, `--base16` flags and `--type` (via `flag::hashAlgo`). Dispatches on mode (`Flat` reads file directly so FIFOs work, `NixArchive` calls `dumpPath`, `Git` uses a recursive `git::DumpHook` via `fun<git::DumpHook>`). Optionally truncates results > 20 bytes via `compressHash`. `description()` varies by mode.
- `CmdHashPath` (struct, derives `CmdHashBase`) — `nix hash path`; constructed with `FileIngestionMethod::NixArchive`; adds `--algo`, `--mode` (via `flag::fileIngestionMethod`), `--format` flags. (`--modulo` flag is `#if 0`-disabled.)
- `CmdHashFile` (struct, derives `CmdHashBase`) — deprecated `nix hash file`; constructed with `FileIngestionMethod::Flat`.
- `CmdToBase` (struct, derives `Command`) — base for `nix hash to-base16/32/64/sri` (and `nix-hash --to-*`). Constructor takes `HashFormat` and `legacyCli` flag; warns when invoked from non-legacy CLI; positional `strings`.
- `CmdHashConvert` (struct, derives `Command`) — `nix hash convert`. Flags `--from`, `--to` (default SRI), `--algo`, positional `hashes`; prints converted forms. `category()` returns `catUtility`.
- `CmdHash` (struct, derives `NixMultiCommand`) — top-level `nix hash`; constructor lists subcommands inline via lambda factories (`convert`, `path`, `file`, `to-base16` (Base16), `to-base32` (Nix32), `to-base64` (Base64), `to-sri` (SRI)); `category()` returns `catUtility`.

### Functions
- `compatNixHash(int argc, char ** argv)` (static) — implements the legacy `nix-hash` CLI; handles `--help`, `--version`, `--flat`, `--base16/--base32/--base64/--sri`, `--truncate`, `--type`, and `--to-base16/-32/-64/-sri`. Constructs `CmdHashBase` (with default `MD5` algo when missing) or `CmdToBase` and calls their `run()`.
- Numerous `description`/`doc`/`category`/`run` member functions on the structs above.

### Command registrations
- `registerCommand<CmdHash>("hash")` — `rCmdHash`.
- `RegisterLegacyCommand r_nix_hash("nix-hash", compatNixHash)` — legacy command registration.
- The seven hash subcommands (`convert`, `path`, `file`, `to-base16`, `to-base32`, `to-base64`, `to-sri`) are registered through the inline `NixMultiCommand` table.

### Type aliases
- None.

### Macros / globals
- None.

## Cross-file observations

- **`registerCommand<...>` boilerplate is uniform.** Every source file ends with one or more `static auto rXxx = registerCommand<CmdXxx>("xxx")` (or `registerCommand2` for two-component paths). Naming is uneven: most are `rCmdXxx`, but `bundle.cc` uses `r2`, `formatter.cc` uses `rFormatterRun`/`rFormatterBuild`/`rFmt`, `config.cc` uses `rShowConfig` for the `config show` registration, and the legacy bridges use `r_nix_hash`/`r_nix_prefetch_url`. The underlying pattern is identical across the 30+ commands defined in this shard.
- **Multi-command parent skeletons split into two styles.** `CmdEnv`, `CmdRealisation`, `CmdDerivation`, `CmdConfig`, `CmdFormatter`, `CmdFlake` all build their child table with `RegisterCommand::getCommandsFor({"<name>"})`, leaving the children to register themselves via `registerCommand2`. By contrast `CmdRegistry`, `CmdProfile`, and `CmdHash` register their children inline via a `{...}` initializer list of name → factory-lambda pairs (and as a result, their child commands are not separately exposed via `registerCommand2`). `CmdProfile` additionally seeds an `aliases` map (`install` → `add`).
- **`getDefaultFlakeAttrPaths`/`getDefaultFlakeAttrPathPrefixes` is copy-pasted.** The `apps.<system>.default` + `defaultApp.<system>` shape appears verbatim in `CmdRun` and `CmdBundle`. `Common` (develop) does the same with `devShells.<system>.default` + `devShell.<system>` (and prefix `devShells.<system>.`). `MixFormatter` returns just `formatter.<system>`. `CmdSearch` returns `packages.<system>` + `legacyPackages.<system>`. Each command also re-walks `SourceExprCommand::getDefaultFlakeAttrPaths()` to merge in the base prefixes — a refactor target for a small `MixFlakeAttrPaths` helper.
- **`parseInstallable(store, _x)` + `Installable::toStorePath(...)` is the standard idiom for raw-string installables.** It appears in `CmdDiffClosures::run` (twice for `_before`/`_after`), `CmdWhyDepends::run` (twice for `_package`/`_dependency`), `Common::makeRcScript` (for `--redirect` resolution), and `CmdFormatterRun`/`CmdFormatterBuild` (for `parseInstallable(store, ".")`). A combined helper could collapse the boilerplate and centralise error mapping.
- **Eval-cache release before `exec*`.** `CmdRun::run`, `CmdDevelop::run`, `CmdShell::run`, `CmdFormatterRun::run` all clear `state->evalCaches` immediately before exec'ing out of the process to make sure caches are flushed without C++ destructor cleanup. The comment "Release our references to eval caches to ensure they are persisted to disk, because we are about to exec out of this process without running C++ destructors." is duplicated verbatim across these sites.
- **JSON-vs-text dual output paths.** `CmdPathInfo::run`, `CmdFlakeMetadata::run`, `CmdFlakeShow::run`, `CmdFlakePrefetch::run`, `CmdFlakeArchive::run`, `CmdRealisationInfo::run`, `CmdConfigShow::run`, `CmdStorePrefetchFile::run`, `CmdProfileList::run`, `CmdSearch::run`, `CmdEval::run`, and `CmdBuild::run` (dry-run path included) all branch on `if (json) { printJSON(...) } else { logger->cout(...) }`. The `printJSON` helper is consistent, but the surrounding control flow could be unified.
- **`MixDefaultProfile` / `MixDryRun` / `MixProfile` mix-ins** appear repeatedly across `CmdBuild`, `CmdProfileAdd`, all `CmdProfile*` siblings, `CmdUpgradeNix`, `CmdAddDerivation`, `CmdProfileRollback`, `CmdProfileWipeHistory`, `CmdProfileUpgrade`, `CmdFlakeArchive`. The pattern of `dryRun` short-circuits (e.g. `dryRun ? computeStorePath : writeDerivation`) is duplicated rather than provided by a helper.
- **Help/`doc()` overrides include `*.md` files via `#include`.** Every command that has a `doc()` consistently does `return ` `#include "name.md"` `;`. This is uniform (sometimes `*.md.gen.hh` instead, e.g. `help-stores`). Suited to a macro/wrapper.
- **`Category` overrides also follow a tight pattern.** `catSecondary`, `catUtility`, `catNixInstallation`, and `catHelp` are spread across many commands; the categorisation is currently encoded in each subclass body rather than in the registration call.
- **Re-implemented argument grammar for legacy commands.** Both `nix-hash` (`compatNixHash`) and `nix-prefetch-url` (`main_nix_prefetch_url`) hand-roll iterator-based `parseCmdLine`/`parseCmdline` flows that look very similar to each other. The `LegacyArgs`/`MixEvalArgs`-derived `MyArgs` helper in `prefetch.cc` is essentially boilerplate. Both also call into `showManPage(...)` for `--help` (defined in `man-pages.cc`) and `printVersion(...)` for `--version`.
- **`Common::makeRcScript` and surrounding env-handling utilities** (`fixupStructuredAttrs`, `BuildEnvironment::toBash`) are unique to `develop.cc` but conceptually similar to `nix env shell`'s PATH manipulation (queueing through `propagated-user-env-packages`); a shared "set up runtime env" helper would unify the two.
- **`treeLast`/`treeConn`/`treeNull`/`treeLine`** ASCII tree glyphs are reused across `CmdFlakeMetadata`, `CmdFlakeShow`, and `CmdWhyDepends` (and through them, in user-visible output). They are defined elsewhere (in libcmd headers) but the *use* pattern (computing `last` from `i + 1 == size` and selecting glyphs) is identical; could be a small helper.
- **`this`-deducing recursive lambdas** (C++23 explicit-object-parameter form) are used in several modern `run` methods: `CmdEval::run` (write-tree recursion), `CmdFlakeMetadata::run` (recursive input tree), `CmdFlakeArchive::run` (`traverse`), `CmdFlakeInitCommon::run` (`copyDir`), and `CmdFlakePrefetchInputs::run` (`visit`). This style replaces older `std::function<...>` self-references and is consistently adopted in the new code.
- **Stable-before-others marker.** `CmdRepl`, `CmdConfigCheck`, `CmdUpgradeNix` all override `experimentalFeature()` to return `nullopt` to opt out of the `Xp::NixCommand` gate; the doc-comment "This command is stable before the others" is identical at all three sites.

