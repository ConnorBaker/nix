# Inventory — Shard 18: nix CLI (store subcommands + legacy)

## File: src/nix/store.cc

### Namespaces
- `nix` — outer namespace.

### Classes / structs / enums
- `CmdStore` (struct, derives `NixMultiCommand`) — multi-command dispatcher for `nix store ...` subcommands; carries deprecated alias `ping → info`; overrides `description`/`category`.

### Functions
- `CmdStore::CmdStore()` — constructor; populates `aliases` map and registers child commands via `RegisterCommand::getCommandsFor({"store"})`.
- `CmdStore::description()` — "manipulate a Nix store".
- `CmdStore::category()` — returns `catUtility`.

### Command / legacy registrations
- `static auto rCmdStore = registerCommand<CmdStore>("store");` — top-level "store" command.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/store-info.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdInfoStore` (struct, derives `StoreCommand, MixJSON`) — implements `nix store info`; reports URL, version, and trust status of a store, optionally as JSON.

### Functions
- `CmdInfoStore::description()` — "test whether a store can be accessed".
- `CmdInfoStore::doc()` — embeds `store-info.md`.
- `CmdInfoStore::run(ref<Store>)` — connects to the store and prints/serialises info; in non-JSON mode emits Store URL, Version, Trusted via `notice`; in JSON mode populates `res["url"|"version"|"trusted"]` printed by a `Finally`.

### Command / legacy registrations
- `static auto rCmdInfoStore = registerCommand2<CmdInfoStore>({"store", "info"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/store-gc.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdStoreGC` (struct, derives `StoreCommand, MixDryRun`) — implements `nix store gc`; holds `GCOptions options` and adds a `--max` flag.

### Functions
- `CmdStoreGC::CmdStoreGC()` — registers the `--max` flag bound to `options.maxFreed`.
- `CmdStoreGC::description()` — "perform garbage collection on a Nix store".
- `CmdStoreGC::doc()` — embeds `store-gc.md`.
- `CmdStoreGC::run(ref<Store>)` — throws `UsageError` if `--max` and `--dry-run` are combined; chooses `gcReturnDead` vs `gcDeleteDead` per `dryRun`, sets `pathsToDelete = WholeStore`, prints freed via `Finally`, and calls `GcStore::collectGarbage`.

### Command / legacy registrations
- `static auto rCmdStoreGC = registerCommand2<CmdStoreGC>({"store", "gc"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/store-delete.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdStoreDelete` (struct, derives `StorePathsCommand`) — implements `nix store delete`; holds a `GCOptions` initialised to `gcDeleteSpecific` plus a `deleteReferrers` toggle.

### Functions
- `CmdStoreDelete::CmdStoreDelete()` — registers `--ignore-liveness`, `--skip-alive` (alias `--skip-live`, sets `options.action = gcDeleteDead`), and `--also-referrers` flags.
- `CmdStoreDelete::description()` — "delete paths from the Nix store".
- `CmdStoreDelete::doc()` — embeds `store-delete.md`.
- `CmdStoreDelete::run(ref<Store>, StorePaths&&)` — bundles paths into `GCOptions::SpecificPaths{paths, deleteReferrers}` and calls `GcStore::collectGarbage` with a `Finally` printing freed bytes.

### Command / legacy registrations
- `static auto rCmdStoreDelete = registerCommand2<CmdStoreDelete>({"store", "delete"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/store-repair.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdStoreRepair` (struct, derives `StorePathsCommand`) — implements `nix store repair`; calls `Store::repairPath` per input.

### Functions
- `CmdStoreRepair::description()` — "repair store paths".
- `CmdStoreRepair::doc()` — embeds `store-repair.md`.
- `CmdStoreRepair::run(ref<Store>, StorePaths&&)` — iterates and calls `repairPath`.

### Command / legacy registrations
- `static auto rStoreRepair = registerCommand2<CmdStoreRepair>({"store", "repair"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/store-copy-log.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdCopyLog` (struct, derives `virtual CopyCommand, virtual InstallablesCommand`) — implements `nix store copy-log`; copies build logs between two `LogStore`s.

### Functions
- `CmdCopyLog::description()` — "copy build logs between Nix stores".
- `CmdCopyLog::doc()` — embeds `store-copy-log.md`.
- `CmdCopyLog::run(ref<Store>, Installables&&)` — for each derivation path returned by `Installable::toDerivations`, fetches the log from the source `LogStore` (`getBuildLog`) and adds it to the destination `LogStore` (`addBuildLog`); throws if the source has no log.

### Command / legacy registrations
- `static auto rCmdCopyLog = registerCommand2<CmdCopyLog>({"store", "copy-log"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/copy.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdCopy` (struct, derives `virtual CopyCommand, virtual BuiltPathsCommand, MixProfile, MixNoCheckSigs`) — implements `nix copy`; copies built paths between stores, optionally creating output symlinks and substituting on the destination; carries `outLink` and `substitute` (default `NoSubstitute`) members.

### Functions
- `CmdCopy::CmdCopy()` — chains `BuiltPathsCommand(true)`, registers `--out-link/-o` (`labels = {"path"}`, `completer = completePath`) and `--substitute-on-destination/-s` (sets `substitute = Substitute`); sets `realiseMode = Realise::Outputs`.
- `CmdCopy::description()` — "copy paths between Nix stores".
- `CmdCopy::doc()` — embeds `copy.md`.
- `CmdCopy::category()` — returns `catSecondary`.
- `CmdCopy::run(ref<Store>, BuiltPaths&& allPaths, BuiltPaths&& rootPaths)` — gathers `RealisedPath::Set` via `BuiltPath::toRealisedPaths`, calls `copyPaths(NoRepair, checkSigs, substitute)`, calls `updateProfile(*dstStore, rootPaths)`, and (if `--out-link` is set) downcasts `dstStore` to `LocalFSStore` and calls `createOutLinks` (or throws).

### Command / legacy registrations
- `static auto rCmdCopy = registerCommand<CmdCopy>("copy");`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/cat.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `MixCat` (struct, derives `virtual Args`) — mixin providing `cat()` helper to stream a single regular file from a `SourceAccessor`.
- `CmdCatStore` (struct, derives `StoreCommand, MixCat`) — implements `nix store cat`; resolves a store-relative path and streams its contents.
- `CmdCatNar` (struct, derives `StoreCommand, MixCat`) — implements `nix nar cat`; streams a regular file from inside a NAR file.
- `CatRegularFileSink` (struct, local to `CmdCatNar::run`, derives `NullFileSystemObjectSink`) — sink with `neededPath` and `found` fields that surfaces only the requested path while still parsing the entire NAR.
- Anonymous local struct (inside `CatRegularFileSink::createRegularFile`, derives `CreateRegularFileSink, FdSink`) — sink instance `crfSink` overriding `isExecutable()` as a no-op; streams to stdout when the requested path matches, else `skipContents`.

### Functions
- `MixCat::cat(ref<SourceAccessor>, CanonPath)` — `lstat`s the path, asserts regular file, stops the logger, writes contents to stdout via `FdSink{getStandardOutput()}` and `accessor->readFile`.
- `CmdCatStore::CmdCatStore()` — declares `path` positional via `expectArgs` with `completePath`.
- `CmdCatStore::description()` — "print the contents of a file in the Nix store on stdout".
- `CmdCatStore::doc()` — embeds `store-cat.md`.
- `CmdCatStore::run(ref<Store>)` — splits store path/rest via `store->toStorePath(path)` and calls `cat(store->requireStoreObjectAccessor(storePath), rest)`.
- `CmdCatNar::CmdCatNar()` — declares `nar` and `path` positionals (the former with `completePath`).
- `CmdCatNar::description()` — "print the contents of a file inside a NAR file on stdout".
- `CmdCatNar::doc()` — embeds `nar-cat.md`.
- `CmdCatNar::run(ref<Store>)` — opens the NAR via `openFileReadonly` and `FdSource`, parses it through `CatRegularFileSink` using `parseDump`, throws if `sink.found` is false.
- `CatRegularFileSink::createRegularFile(const CanonPath&, fun<void(CreateRegularFileSink&)>)` — overrides `NullFileSystemObjectSink::createRegularFile`; if `path == neededPath`, stops logger, sets `crfSink.fd = getStandardOutput()`, `skipContents = false`, and `found = true`; otherwise sets `skipContents = true`.

### Command / legacy registrations
- `static auto rCmdCatStore = registerCommand2<CmdCatStore>({"store", "cat"});`.
- `static auto rCmdCatNar = registerCommand2<CmdCatNar>({"nar", "cat"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/ls.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `MixLs` (struct, derives `virtual Args, MixJSON`) — mixin providing `recursive`/`verbose`/`showDirectory` flags plus a text and JSON listing helper.
- `CmdLsStore` (struct, derives `StoreCommand, MixLs`) — implements `nix store ls`; takes a store path and lists.
- `CmdLsNar` (struct, derives `Command, MixLs`) — implements `nix nar ls`; takes a NAR file and a path inside it.

### Functions
- `MixLs::MixLs()` — adds `--recursive/-R`, `--long/-l` (sets `verbose`), `--directory/-d` (sets `showDirectory`).
- `MixLs::listText(ref<SourceAccessor>, CanonPath)` — recursive textual directory walker using nested lambdas `showFile` and `doPath`; verbose mode prints pseudo-permissions strings (`-r-xr-xr-x`, `-r--r--r--`, `lrwxrwxrwx`, `dr-xr-xr-x`), filesize, and symlink targets.
- `MixLs::list(ref<SourceAccessor>, CanonPath)` — JSON branch (rejects `--directory`, dispatches to `listNarShallow`/`listNarDeep` and prints `j.dump()`); otherwise calls `listText`.
- `CmdLsStore::CmdLsStore()` — declares `path` positional via `expectArgs` with `completePath`.
- `CmdLsStore::description()` — "show information about a path in the Nix store".
- `CmdLsStore::doc()` — embeds `store-ls.md`.
- `CmdLsStore::run(ref<Store>)` — splits store path/rest via `store->toStorePath(path)` and calls `list(store->requireStoreObjectAccessor(storePath), rest)`.
- `CmdLsNar::CmdLsNar()` — declares `nar` (with `completePath`) and `path` positionals.
- `CmdLsNar::doc()` — embeds `nar-ls.md`.
- `CmdLsNar::description()` — "show information about a path inside a NAR file".
- `CmdLsNar::run()` — opens NAR via `openFileReadonly` + `FdSource`, builds a lazy NAR accessor via `makeLazyNarAccessor(parseNarListing(source), seekableGetNarBytes(fd.get()))`, then calls `list`.

### Command / legacy registrations
- `static auto rCmdLsStore = registerCommand2<CmdLsStore>({"store", "ls"});`.
- `static auto rCmdLsNar = registerCommand2<CmdLsNar>({"nar", "ls"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/nar.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdNar` (struct, derives `NixMultiCommand`) — multi-command dispatcher for `nix nar ...` subcommands.

### Functions
- `CmdNar::CmdNar()` — registers child commands via `RegisterCommand::getCommandsFor({"nar"})`.
- `CmdNar::description()` — "create or inspect NAR files".
- `CmdNar::doc()` — embeds `nar.md`.
- `CmdNar::category()` — returns `catUtility`.

### Command / legacy registrations
- `static auto rCmdNar = registerCommand<CmdNar>("nar");`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/dump-path.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdDumpPath` (struct, derives `StorePathCommand`) — implements `nix store dump-path`; serialises a store path to NAR on stdout.
- `CmdDumpPath2` (struct, derives `Command`) — implements `nix nar pack`; serialises an arbitrary path to NAR on stdout.
- `CmdNarDumpPath` (struct, derives `CmdDumpPath2`) — deprecated alias warning wrapper for `nix nar dump-path`.

### Functions
- `getNarSink()` (static) — returns `FdSink(std::move(getStandardOutput()))`, throwing `UsageError` if stdout is a TTY.
- `CmdDumpPath::description()` — "serialise a store path to stdout in NAR format".
- `CmdDumpPath::doc()` — embeds `store-dump-path.md`.
- `CmdDumpPath::run(ref<Store>, const StorePath&)` — calls `Store::narFromPath` into `getNarSink()` and flushes.
- `CmdDumpPath2::CmdDumpPath2()` — declares `path` positional (with `completePath`).
- `CmdDumpPath2::description()` — "serialise a path to stdout in NAR format".
- `CmdDumpPath2::doc()` — embeds `nar-dump-path.md`.
- `CmdDumpPath2::run()` — calls `dumpPath(path, sink)` and flushes.
- `CmdNarDumpPath::run()` — emits deprecation `warn` then delegates to `CmdDumpPath2::run`.

### Command / legacy registrations
- `static auto rDumpPath = registerCommand2<CmdDumpPath>({"store", "dump-path"});`.
- `static auto rCmdNarPack = registerCommand2<CmdDumpPath2>({"nar", "pack"});`.
- `static auto rCmdNarDumpPath = registerCommand2<CmdNarDumpPath>({"nar", "dump-path"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/add-to-store.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdAddToStore` (struct, derives `MixDryRun, StoreCommand`) — base helper that adds a file/directory to the store; carries `path`, `namePart`, `caMethod` (default `NixArchive`), and `hashAlgo` (default `SHA256`).
- `CmdAdd` (struct, derives `CmdAddToStore`) — `nix store add` (current name).
- `CmdAddFile` (struct, derives `CmdAddToStore`) — deprecated `nix store add-file`; pre-sets `caMethod = ContentAddressMethod::Raw::Flat`.
- `CmdAddPath` (struct, derives `CmdAddToStore`) — deprecated alias `nix store add-path` for `nix store add`.

### Functions
- `CmdAddToStore::CmdAddToStore()` — `expectArg("path", &path)`; registers `--name/-n` flag (label `"name"`); adds reusable flags via `flag::contentAddressMethod(&caMethod)` and `flag::hashAlgo(&hashAlgo)`.
- `CmdAddToStore::run(ref<Store>)` — defaults `namePart` to `path.filename()`; opens a `makeFSSourceAccessor(absPath(path))`; if `dryRun` calls `computeStorePath` else `addToStoreSlow`; prints the store path.
- `CmdAdd::description()` — "Add a file or directory to the Nix store".
- `CmdAdd::doc()` — embeds `add.md`.
- `CmdAddFile::CmdAddFile()` — sets `caMethod = ContentAddressMethod::Raw::Flat`.
- `CmdAddFile::description()` — deprecation message pointing to `nix store add --mode flat`.
- `CmdAddPath::description()` — deprecation message pointing to `nix store add`.

### Command / legacy registrations
- `static auto rCmdAddFile = registerCommand2<CmdAddFile>({"store", "add-file"});`.
- `static auto rCmdAddPath = registerCommand2<CmdAddPath>({"store", "add-path"});`.
- `static auto rCmdAdd = registerCommand2<CmdAdd>({"store", "add"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/make-content-addressed.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdMakeContentAddressed` (struct, derives `virtual CopyCommand, virtual StorePathsCommand, MixJSON`) — implements `nix store make-content-addressed`; rewrites paths/closures to a content-addressed form, optionally outputting a JSON `rewrites` map.

### Functions
- `CmdMakeContentAddressed::CmdMakeContentAddressed()` — sets `realiseMode = Realise::Outputs`.
- `CmdMakeContentAddressed::description()` — "rewrite a path or closure to content-addressed form".
- `CmdMakeContentAddressed::doc()` — embeds `make-content-addressed.md`.
- `CmdMakeContentAddressed::run(ref<Store>, StorePaths&&)` — opens dst store via `openStore()` (default) or `openStore(StoreReference{*dstUri})`, calls `makeContentAddressed`, prints `rewrites` JSON object via `printJSON` or human `notice` per entry.

### Command / legacy registrations
- `static auto rCmdMakeContentAddressed = registerCommand2<CmdMakeContentAddressed>({"store", "make-content-addressed"});`.

### Type aliases
- `using nlohmann::json;` (file-scope using-declaration).

### Macros / globals
- (none)

---

## File: src/nix/optimise-store.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdOptimiseStore` (struct, derives `StoreCommand`) — implements `nix store optimise`; calls `Store::optimiseStore`.

### Functions
- `CmdOptimiseStore::description()` — "replace identical files in the store by hard links".
- `CmdOptimiseStore::doc()` — embeds `optimise-store.md`.
- `CmdOptimiseStore::run(ref<Store>)` — calls `store->optimiseStore()`.

### Command / legacy registrations
- `static auto rCmdOptimiseStore = registerCommand2<CmdOptimiseStore>({"store", "optimise"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/sigs.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdCopySigs` (struct, derives `StorePathsCommand`) — implements `nix store copy-sigs`; pulls signatures from substituters in parallel.
- `CmdSign` (struct, derives `StorePathsCommand`) — implements `nix store sign`; signs paths with a local secret key.
- `CmdKeyGenerateSecret` (struct, derives `Command`) — implements `nix key generate-secret`.
- `CmdKeyConvertSecretToPublic` (struct, derives `Command`) — implements `nix key convert-secret-to-public`.
- `CmdKey` (struct, derives `NixMultiCommand`) — multi-command dispatcher for `nix key ...`; statically enumerates its children rather than using `RegisterCommand::getCommandsFor`.

### Functions
- `CmdCopySigs::CmdCopySigs()` — registers `--substituter/-s` (multi, parses each as `StoreReference::parse`).
- `CmdCopySigs::description()` — "copy store path signatures from substituters".
- `CmdCopySigs::doc()` — embeds `store-copy-sigs.md`.
- `CmdCopySigs::run(ref<Store>, StorePaths&&)` — throws if no substituters, opens substituter stores, runs `ThreadPool` (sized by `fileTransferSettings.httpConnections`) per path, equality-checks `narHash`/`narSize`/`references` before adopting any new signatures, calls `addSignatures`, prints "imported %d signatures".
- `CmdSign::CmdSign()` — registers `--key-file/-k` (required) with `completePath`.
- `CmdSign::description()` — "sign store paths with a local key".
- `CmdSign::run(ref<Store>, StorePaths&&)` — constructs `LocalSigner` from `SecretKey(readFile(secretKeyFile))`; for each path, copies info, clears sigs, signs, and `addSignatures` if signature is new; prints "added %d signatures".
- `CmdKeyGenerateSecret::CmdKeyGenerateSecret()` — registers `--key-name` (required, label `"name"`).
- `CmdKeyGenerateSecret::description()` — "generate a secret key for signing store paths".
- `CmdKeyGenerateSecret::doc()` — embeds `key-generate-secret.md`.
- `CmdKeyGenerateSecret::run()` — stops the logger and writes `SecretKey::generate(keyName).to_string()` to stdout via `writeFull`.
- `CmdKeyConvertSecretToPublic::description()` — "generate a public key for verifying store paths from a secret key read from standard input".
- `CmdKeyConvertSecretToPublic::doc()` — embeds `key-convert-secret-to-public.md`.
- `CmdKeyConvertSecretToPublic::run()` — reads stdin via `drainFD`, stops the logger, writes `secretKey.toPublicKey().to_string()` via `writeFull`.
- `CmdKey::CmdKey()` — constructs `NixMultiCommand` with hard-coded child factories for `generate-secret` and `convert-secret-to-public`.
- `CmdKey::description()` — "generate and convert Nix signing keys".
- `CmdKey::category()` — returns `catUtility`.

### Command / legacy registrations
- `static auto rCmdCopySigs = registerCommand2<CmdCopySigs>({"store", "copy-sigs"});`.
- `static auto rCmdSign = registerCommand2<CmdSign>({"store", "sign"});`.
- `static auto rCmdKey = registerCommand<CmdKey>("key");`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/verify.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdVerify` (struct, derives `StorePathsCommand`) — implements `nix store verify`; checks contents and trust against substituters in parallel and exits with bitmask status; carries `noContents`, `noTrust`, `substituterUris`, `sigsNeeded`.

### Functions
- `CmdVerify::CmdVerify()` — registers `--no-contents`, `--no-trust`, `--substituter/-s` (multi, parses with `StoreReference::parse`), `--sigs-needed/-n` (label `"n"`).
- `CmdVerify::description()` — "verify the integrity of store paths".
- `CmdVerify::doc()` — embeds `verify.md`.
- `CmdVerify::run(ref<Store>, StorePaths&&)` — opens substituters, fetches `getDefaultPublicKeys`, uses `Activity actVerifyPaths` and a `ThreadPool` to compute NAR hashes (`HashSink`) and aggregate signatures (using `ValidPathInfo::maxSigs` cap and treating content-addressed paths as fully signed); per-path inner `doSigs` lambda dedupes signatures; throws `Exit((corrupted?1:0)|(untrusted?2:0)|(failed?4:0))`.

### Command / legacy registrations
- `static auto rCmdVerify = registerCommand2<CmdVerify>({"store", "verify"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/build-remote/build-remote.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (none — single-file legacy entry point.)

### Functions
- `handleAlarm(int sig)` (static) — empty signal handler used to interrupt `lockFile` blocking waits via `alarm`.
- `escapeUri(std::string)` — replaces `/` with `_` to make a URI safe as a filename.
- `openSlotLock(const Machine &, uint64_t slot)` (static) — opens an exclusive `currentLoad/<escapedUri>-<slot>` lock file via `openLockFile`.
- `allSupportedLocally(Store&, const StringSet & requiredFeatures)` (static) — true iff every required feature is in the local store's `systemFeatures`.
- `main_build_remote(int argc, char ** argv)` (static, returns `int`) — resets/unblocks SIGTERM, switches `logger` to JSON, scrubs `DISPLAY`/`SSH_ASKPASS`, reads `verbosity` from argv, parses parent settings + per-build inputs from stdin, sets up `currentLoad` directory (per-machine slot locks), parses `Machine::parseConfig`, then for each build attempt: finds best machine slot (combining `enabled`/`systemSupported`/`allSupported`/`mandatoryMet` checks weighed by `speedFactor` and `load`), takes upload lock with 15-minute alarm, opens an SSH store, copies dependencies (honouring `buildersUseSubstitutes`), runs `buildDerivation` (trusted/CA case, hijacking `inputSrcs`) or `buildPathsWithResults` (otherwise), then copies missing outputs back and registers any new realisations.

### Command / legacy registrations
- `static RegisterLegacyCommand r_build_remote("build-remote", main_build_remote);`.

### Type aliases
- (none)

### Macros / globals
- `static std::filesystem::path currentLoad;` — directory holding per-machine slot locks.

---

## File: src/nix/nix-build/nix-build.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- Local struct `MyArgs` (declared inside `main_nix_build`, derives `LegacyArgs, MixEvalArgs`) — adds a `setBaseDir` helper for shebang-relative path resolution; reuses `LegacyArgs::LegacyArgs`.

### Functions
- `shellwords(std::string_view s)` (static) — perl-shellwords-style tokenizer with single/double-quote and backslash handling, used for `#!nix-shell` lines; uses local `enum state { sBegin, sSingleQuote, sDoubleQuote }`.
- `resolveShellExprPath(SourcePath path)` (static) — variant of `resolveExprPath` that prefers `shell.nix` (when `compatibilitySettings.nixShellAlwaysLooksForShellNix` is set, otherwise warns and falls back), then `default.nix`, throwing if neither exists.
- `main_nix_build(int argc, char ** argv)` (static, returns `void`) — entry point shared by `nix-build` and `nix-shell`; handles shebang interpretation, `--pure`/`--impure`, `--packages/-p`, `--add-drv-link`/`--indirect` (obsolete), `--no-out-link`/`--no-link`, `--attr/-A`, `--out-link/-o`, `--dry-run`, `--check`, `--exclude`, `--expr/-E`, `--keep`, `-i` (shebang interpreter), `--command`/`--run`, then either builds derivations and writes `result-*` symlinks (via `LocalFSStore::addPermRoot`), or assembles a bash rcfile (handling `pure` env scrubbing, structured attributes via `StructuredAttrs::writeShell`, `NIX_BUILD_TOP`/`NIX_BUILD_CORES` etc.) and `execvp`s a shell.
- Inner lambda `accumDerivedPath` (`this auto& self, ref<SingleDerivedPath>, const DerivedPathMap<StringSet>::ChildNode&`) — recursively builds the list of `DerivedPath::Built{drvPath, outputs}` for shell mode.
- Inner `fun<...>` `accumInputClosure(const StorePath&, const DerivedPathMap<StringSet>::ChildNode&)` — accumulates `inputs` for `StructuredAttrs::prepareStructuredAttrs`/`writeShell`.
- Inner lambda `buildPaths(const std::vector<DerivedPath>&)` — wrapper that prints missing paths (when `settings.printMissing`) and calls `Store::buildPaths(paths, buildMode, evalStore)` unless `dryRun`.
- Inner lambda `takesNixShellAttr(const Value&)` — true iff a function value has an `inNixShell` formal parameter.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_build("nix-build", main_nix_build);`.
- `static RegisterLegacyCommand r_nix_shell("nix-shell", main_nix_build);`.

### Type aliases
- `using namespace std::string_literals;` (file scope).

### Macros / globals
- `extern char ** environ __attribute__((weak));` — used to install the rebuilt environment for `execvp`.

---

## File: src/nix/nix-channel/nix-channel.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum { cNone, cAdd, cRemove, cList, cUpdate, cListGenerations, cRollback }` — local command discriminator inside `main_nix_channel`.

### Functions
- `readChannels()` (static) — parses `~/.nix-channels`-style file into the `channels` map (skips comments, normalises trailing slashes, falls back to URL basename for unnamed channels).
- `writeChannels()` (static) — rewrites the channels file from the in-memory map via `openNewFileForWrite`.
- `addChannel(const std::string & url, const std::string & name)` (static) — validates URL regex (`file|http|https`) and name regex, mutates `channels`, persists.
- `removeChannel(const std::string & name)` (static) — drops a channel, persists, runs `nix-env --uninstall`.
- `update(const StringSet & channelNames)` (static) — for each subscribed channel, downloads the tarball via `fetchers::downloadFile` or reuses a previous closure, generates an evaluation expression appending any version match in the URL basename, runs `nix-env --install --remove-all --from-expression` against the channels profile, and refreshes `~/.nix-defexpr/channels` (replacing legacy symlink).
- `main_nix_channel(int argc, char ** argv)` (static, returns `int`) — sets `channelsList` (XDG vs `~/.nix-channels`), `nixDefExpr`, and `profile` paths, parses `--add/--remove/--list/--update/--list-generations/--rollback` and dispatches.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_channel("nix-channel", main_nix_channel);`.

### Type aliases
- `typedef StringMap Channels;` — name → URL map.

### Macros / globals
- `static Channels channels;` — in-memory channel list.
- `static std::filesystem::path channelsList;` — path of `~/.nix-channels` (or XDG variant).
- `static std::filesystem::path profile;` — `channels` profile path.
- `static std::filesystem::path nixDefExpr;` — `~/.nix-defexpr` path.

---

## File: src/nix/nix-collect-garbage/nix-collect-garbage.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (none)

### Functions
- `removeOldGenerations(std::filesystem::path dir)` — recursively walks profile directories (read/write checks, skips on `ENOENT`), and on symlinks whose target contains "link" calls `deleteOldGenerations` or `deleteGenerationsOlderThan`.
- `main_nix_collect_garbage(int argc, char ** argv)` (static, returns `int`) — parses `--delete-old/-d`, `--delete-older-than`, `--dry-run`, `--max-freed` (clamped non-negative), throws if `--max-freed` combined with `--dry-run`, optionally walks profiles (legacy and XDG) to delete old generations, then opens store and calls `GcStore::collectGarbage` with `WholeStore{}` and a `Finally` printing freed bytes.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_collect_garbage("nix-collect-garbage", main_nix_collect_garbage);`.

### Type aliases
- (none)

### Macros / globals
- `std::string deleteOlderThan;` — file-scope global parameter for `--delete-older-than`.
- `bool dryRun = false;` — file-scope dry-run flag.

---

## File: src/nix/nix-copy-closure/nix-copy-closure.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (none)

### Functions
- `main_nix_copy_closure(int argc, char ** argv)` (static, returns `int`) — parses `--gzip/--bzip2/--xz` (warns and falls back to gzip for non-gzip), `--from/--to` (defaults `to`), `--include-outputs`, `--show-progress` (warns "not implemented"), `--dry-run`, `--use-substitutes/-s`, then constructs a `LegacySSHStoreConfig` from the SSH host, opens both ends, and calls `copyClosure(NoRepair, NoCheckSigs, useSubstitutes)`.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_copy_closure("nix-copy-closure", main_nix_copy_closure);`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/nix-env/nix-env.cc

### Namespaces
- `nix` plus an unnamed inner namespace (for `searchByPrefix`, `Match`, `pickNewestOnly`).

### Classes / structs / enums
- `EnvSettings` (struct, derives `Config`) — one `Setting<bool> keepDerivations` named `keep-env-derivations` (alias `env-keep-derivations`), defaulting to `false`.
- `enum InstallSourceType { srcNixExprDrvs, srcNixExprs, srcStorePaths, srcProfile, srcAttrPath, srcUnknown };` — install-source discriminator (declared via `typedef enum`).
- `InstallSourceInfo` (struct) — `{type, nixExprPath (shared_ptr<SourcePath>), profile, systemFilter, autoArgs (Bindings*)}`.
- `Globals` (struct) — operation-wide context: `instSource`, `profile`, `state`, `dryRun`, `preserveInstalled`, `removeAll`, `forceName`, `prebuiltOnly`.
- `Match` (struct, in anonymous namespace) — pair of `PackageInfo packageInfo` and `std::size_t index`, used during selector resolution.
- `enum UpgradeType { utLt, utLeq, utEq, utAlways };` — upgrade comparison policy (declared via `typedef enum`).
- `enum VersionDiff { cvLess, cvEqual, cvGreater, cvUnavail };` — output of `compareVersionAgainstSet` (declared via `typedef enum`).
- Local in-function enum `enum { sInstalled, sAvailable }` (inside `opQuery`).
- Local struct `MyArgs : LegacyArgs, MixEvalArgs` (inside `main_nix_env`); reuses `LegacyArgs::LegacyArgs`.

### Functions
- `needArg(Strings::iterator & i, Strings & args, const std::string & arg)` (static, returns `std::string`) — pop the next arg or throw.
- `parseInstallSourceOptions(Globals&, Strings::iterator&, Strings&, const std::string&)` (static, returns `bool`) — handles `--from-expression/-E`, `--from-profile`, `--attr/-A`; returns `true` if consumed.
- `isNixExpr(const SourcePath&, struct SourceAccessor::Stat&)` (static) — true if regular file or directory containing `default.nix`.
- `getAllExprs(EvalState&, const SourcePath&, StringSet & seen, BindingsBuilder & attrs)` (static) — recursively builds attrset of `import` calls for `~/.nix-defexpr` style trees, skipping `manifest.nix`, capping at `maxAttrs = 1024`; warns on name collisions.
- `loadSourceExpr(EvalState&, const SourcePath&, Value&)` (static) — single file or recursively merged directory; injects `_combineChannels` into the bindings.
- `loadDerivations(EvalState&, const SourcePath&, std::string systemFilter, Bindings&, const std::string & pathPrefix, PackageInfos&)` (static) — loads and filters derivations matching `systemFilter`.
- `getPriority(EvalState&, PackageInfo&)` (static, returns `NixInt`) — reads `meta.priority` (default `0`).
- `comparePriorities(EvalState&, PackageInfo&, PackageInfo&)` (static, returns `std::strong_ordering`) — three-way comparison via `<=>` on priorities (lower number wins; arguments swapped).
- `isPrebuilt(EvalState&, PackageInfo&)` (static) — true if path is valid or substitutable.
- `checkSelectorUse(DrvNames&)` (static) — throws if a non-`*` selector matched nothing.
- `searchByPrefix(const PackageInfos&, std::string_view prefix)` (anon namespace) — up to `maxResults = 3` name suggestions.
- `pickNewestOnly(EvalState&, std::vector<Match>)` (anon namespace) — selects best per-name match by system / priority / version; warns on ties.
- `filterBySelector(EvalState&, const PackageInfos&, const Strings&, bool newestOnly)` (static) — runs `DrvNames` selectors, optionally calls `pickNewestOnly`, suggests prefixes when nothing matches.
- `isPath(std::string_view)` (static) — `s` contains a `/`.
- `queryInstSources(EvalState&, InstallSourceInfo&, const Strings&, PackageInfos&, bool newestOnly)` (static) — dispatches per `InstallSourceType` to gather candidate `PackageInfos`.
- `printMissing(EvalState&, PackageInfos&)` (static) — adapter that maps `PackageInfos` to `DerivedPath`s and calls `nix::printMissing(state.store, targets)`.
- `keep(PackageInfo&)` (static) — reads `meta.keep` (default `false`).
- `setMetaFlag(EvalState&, PackageInfo&, const std::string& name, const std::string& value)` (static) — mutates a string meta attribute.
- `installDerivations(Globals&, const Strings&, const std::filesystem::path&, std::optional<int> priority)` (static) — main worker for `--install`; honours `forceName`, `prebuiltOnly`, `preserveInstalled`, `removeAll`; loops on `optimisticLockProfile` and calls `createUserEnv`.
- `opInstall(Globals&, Strings, Strings)` (static) — `--install/-i` operation handler; parses `--preserve-installed/-P`, `--remove-all/-r`, `--priority`.
- `upgradeDerivations(Globals&, const Strings&, UpgradeType)` (static) — main worker for `--upgrade`; finds higher-version match per name (subject to priority), prints upgrading/downgrading.
- `opUpgrade(Globals&, Strings, Strings)` (static) — `--upgrade/-u` handler; parses `--lt`, `--leq`, `--eq`, `--always`.
- `opSetFlag(Globals&, Strings, Strings)` (static) — `--set-flag` handler.
- `opSet(Globals&, Strings, Strings)` (static) — `--set` handler (replaces the entire profile content with one drv via `createGeneration`/`switchLink`); only supported on `LocalFSStore`.
- `uninstallDerivations(Globals&, Strings&, const std::filesystem::path&)` (static) — main worker for `--uninstall`; supports both store-path and selector forms.
- `opUninstall(Globals&, Strings, Strings)` (static) — `--uninstall/-e` handler.
- `cmpChars(char a, char b)` (static) — case-insensitive comparator used by `cmpElemByName`.
- `cmpElemByName(const PackageInfo&, const PackageInfo&)` (static) — alphabetic name comparator.
- `compareVersionAgainstSet(const PackageInfo&, const PackageInfos&, std::string& version)` (static, returns `VersionDiff`) — figures out where this element sits relative to a set of other versions.
- `queryJSON(Globals&, std::vector<PackageInfo>&, bool printOutPath, bool printDrvPath, bool printMeta)` (static) — emits the `--query --json` output with `name`/`pname`/`version`/`system`/`outputName`/`outputs`/optionally `drvPath` and `meta`.
- `opQuery(Globals&, Strings, Strings)` (static) — `--query/-q` handler with table/XML/JSON branches; supports `--status/-s`, `--no-name`, `--system`, `--description`, `--compare-versions/-c`, `--drv-path`, `--out-path`, `--meta`, `--installed`, `--available/-a`, `--xml`, `--json`, `--attr-path/-P`, `--attr/-A`.
- `opSwitchProfile(Globals&, Strings, Strings)` (static) — `--switch-profile/-S`; symlinks `~/.nix-profile` (or XDG variant) to the new profile.
- `opSwitchGeneration(Globals&, Strings, Strings)` (static) — `--switch-generation/-G`.
- `opRollback(Globals&, Strings, Strings)` (static) — `--rollback`.
- `opListGenerations(Globals&, Strings, Strings)` (static) — `--list-generations`.
- `opDeleteGenerations(Globals&, Strings, Strings)` (static) — `--delete-generations` (`old`, `Nd`, `+N`, or specific list).
- `opVersion(Globals&, Strings, Strings)` (static) — `--version`.
- `main_nix_env(int argc, char ** argv)` (static, returns `int`) — top-level dispatcher; sets up `nixDefExpr`/profile defaults, installs default channel/channels_root symlinks if missing, parses general flags (`--profile/-p`, `--file/-f`, `--system-filter`, `--prebuilt-only/-b`, `--dry-run`, `--force-name`) plus per-operation flags collected into `opFlags`, opens store + `EvalState`, then calls the chosen `op`.

### Command / legacy registrations
- `static GlobalConfig::Register rEnvSettings(&envSettings);`.
- `static RegisterLegacyCommand r_nix_env("nix-env", main_nix_env);`.

### Operation handler table (12 entries)
| Flag(s) | Handler |
|---|---|
| `--install`, `-i` | `opInstall` |
| `--uninstall`, `-e` | `opUninstall` |
| `--upgrade`, `-u` | `opUpgrade` |
| `--set-flag` | `opSetFlag` |
| `--set` | `opSet` |
| `--query`, `-q` | `opQuery` |
| `--switch-profile`, `-S` | `opSwitchProfile` |
| `--switch-generation`, `-G` | `opSwitchGeneration` |
| `--rollback` | `opRollback` |
| `--list-generations` | `opListGenerations` |
| `--delete-generations` | `opDeleteGenerations` |
| `--version` | `opVersion` |

### Type aliases
- `typedef void (*Operation)(Globals & globals, Strings opFlags, Strings opArgs);` — operation function pointer.
- `typedef enum { srcNixExprDrvs, srcNixExprs, srcStorePaths, srcProfile, srcAttrPath, srcUnknown } InstallSourceType;`.
- `typedef enum { utLt, utLeq, utEq, utAlways } UpgradeType;`.
- `typedef enum { cvLess, cvEqual, cvGreater, cvUnavail } VersionDiff;`.

### Macros / globals
- `EnvSettings envSettings;` — global instance.
- `static constexpr size_t maxAttrs = 1024;` — directory expression cap.

---

## File: src/nix/nix-env/user-env.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- (none)

### Functions
- `queryInstalled(EvalState&, const std::filesystem::path & userEnv)` — rejects `manifest.json` profiles (errors and points to `nix profile`); parses `manifest.nix` via `evalFile` + `getDerivations`; returns `PackageInfos`.
- `createUserEnv(EvalState&, PackageInfos&, const std::filesystem::path & profile, bool keepDerivations, const std::string & lockToken)` — builds inputs (`store->buildPaths` from `queryDrvPath`); constructs the manifest list (per element: `type=derivation`, `name`, `system`, `outPath`, optional `drvPath`, `outputs`, `meta`) via `state.buildBindings`; serialises the manifest and adds it to the store as `env-manifest.nix`; evaluates the embedded `buildenv.nix` to produce a top-level derivation; builds it; if the store is `LocalFSStore`, takes a `PathLocks` lock, checks `optimisticLockProfile` against `lockToken` (returns `false` to retry on mismatch), then calls `createGeneration` and `switchLink`; returns `true` otherwise.

### Command / legacy registrations
- (none — pure helpers used by `nix-env`.)

### Type aliases
- (none)

### Macros / globals
- (none)

---

## File: src/nix/nix-env/user-env.hh

### Namespaces
- `nix`.

### Functions (declared)
- `queryInstalled(EvalState&, const std::filesystem::path & userEnv)`.
- `createUserEnv(EvalState&, PackageInfos&, const std::filesystem::path & profile, bool keepDerivations, const std::string & lockToken)`.

### Macros / globals
- `#pragma once` plus `///@file` doc marker.

---

## File: src/nix/nix-instantiate/nix-instantiate.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum OutputKind { okPlain, okRaw, okXML, okJSON };` — selects evaluation output format.
- Local struct `MyArgs : LegacyArgs, MixEvalArgs` (inside `main_nix_instantiate`); reuses `LegacyArgs::LegacyArgs`.

### Functions
- `processExpr(EvalState&, const Strings& attrPaths, bool parseOnly, bool strict, Bindings & autoArgs, bool evalOnly, OutputKind, bool location, Expr * e)` — `parseOnly` prints the AST via `Expr::show`; otherwise evaluates and either emits the value (raw via `coerceToString`, XML via `printValueAsXML`, JSON via `printValueAsJSON`, or ambiguous text via `printAmbiguous`), or instantiates derivations and prints `.drv` paths optionally rooted via `LocalFSStore::addPermRoot` (printing `printGCWarning` when `gcRoot` is empty); calls `state.ensureLazyPathsCopied(context)` per attrPath.
- `main_nix_instantiate(int argc, char ** argv)` (static, returns `int`) — parses CLI options (`--expr/-E`, `--eval`/`--eval-only`, `--read-write-mode`, `--parse`/`--parse-only`, `--find-file`, `--attr/-A`, `--add-root`, `--indirect` (no-op), `--raw`, `--xml`, `--json`, `--no-location`, `--strict`, `--dry-run`), opens stores (eval store optional via `evalStoreUrl`), parses each input file or `-` (stdin) into `Expr*`, calls `processExpr` for each, defaults to `./default.nix` when no input given.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_instantiate("nix-instantiate", main_nix_instantiate);`.

### Type aliases
- (none)

### Macros / globals
- `std::filesystem::path gcRoot;` — file-scope `--add-root` value (non-static, external linkage).
- `static int rootNr = 0;` — counter for deduplicating GC root link names.

---

## File: src/nix/nix-store/dotgraph.cc

### Namespaces
- `nix`.

### Functions
- `dotQuote(std::string_view)` (static, returns `std::string`) — wraps in literal quotes.
- `nextColour()` (static, returns `const std::string &`) — round-robin from local statics `n` and `colours = {"black","red","green","blue","magenta","burlywood"}`.
- `makeEdge(std::string_view src, std::string_view dst)` (static) — DOT edge with cycling colour.
- `makeNode(std::string_view id, std::string_view label, std::string_view colour)` (static) — DOT node line with `shape=box, style=filled`.
- `printDotGraph(ref<Store>, StorePathSet && roots)` — BFS over `references` (`workList.extract(begin())`/`doneSet`), emitting `digraph G { ... }` with red-filled nodes; reverses edges (writes `references[p] -> path`).

### Command / legacy registrations
- (none)

### Type aliases
- `using std::cout;` (file scope).

### Macros / globals
- (none)

---

## File: src/nix/nix-store/dotgraph.hh

### Namespaces
- `nix`.

### Functions (declared)
- `printDotGraph(ref<Store> store, StorePathSet && roots);`.

### Macros / globals
- `#pragma once`, `///@file`.

---

## File: src/nix/nix-store/graphml.cc

### Namespaces
- `nix`.

### Functions
- `xmlQuote(std::string_view)` (static inline, returns `std::string_view`) — pass-through; comment notes store paths don't need quoting.
- `symbolicName(std::string_view)` (static) — returns the prefix up to and including the first `-`.
- `makeNode(const ValidPathInfo&)` (static) — emits a `<node>` element with `narSize`, `name` (symbolic), `type=derivation|output-path` data keys.
- `printGraphML(ref<Store>, StorePathSet && roots)` — emits GraphML XML preamble (`xmlns`, `xsi:schemaLocation`, three `<key>` definitions, `<graph edgedefault='directed'>`), walks the references closure with the same `workList`/`doneSet` BFS, defines a local lambda `makeEdge(src, dst)` returning `<edge source target/>`, closes `</graph></graphml>`.

### Command / legacy registrations
- (none)

### Type aliases
- `using std::cout;` (file scope).

### Macros / globals
- (none)

---

## File: src/nix/nix-store/graphml.hh

### Namespaces
- `nix`.

### Functions (declared)
- `printGraphML(ref<Store> store, StorePathSet && roots);`.

### Macros / globals
- `#pragma once`, `///@file`.

---

## File: src/nix/nix-store/nix-store.cc

### Namespaces
- `nix_store` (note: not `nix`); imports `nix` via `using namespace nix;`.

### Classes / structs / enums
- `enum QueryType { qOutputs, qRequisites, qReferences, qReferrers, qReferrersClosure, qDeriver, qValidDerivers, qBinding, qHash, qSize, qTree, qGraph, qGraphML, qResolve, qRoots };` — `--query` sub-mode (declared inside `opQuery`).

### Functions
- `ensureLocalStore()` (returns `ref<LocalStore>`) — downcasts the global `store` to `LocalStore`, throwing if not local.
- `useDeriver(const StorePath&)` (static) — returns the path itself if it is a `.drv`, else its known deriver from `queryPathInfo`.
- `realisePath(StorePathWithOutputs path, bool build = true)` (static, returns `std::set<std::filesystem::path>`) — for derivations, optionally builds, then returns the GC-rooted output paths via `LocalFSStore::addPermRoot` (with `result-N`/`-output` naming); for non-drvs, ensures the path exists (or rejects unknowns) and optionally adds a permanent root; warns via `printGCWarning` when `gcRoot` is unset.
- `opRealise(Strings opFlags, Strings opArgs)` (static) — `--realise/-r`/`--realize`, supports `--dry-run`, `--repair`, `--check`, `--ignore-unknown`; calls `Store::queryMissing`/`buildPaths`.
- `opAdd(Strings, Strings)` (static) — `--add/-A`; calls `addToStore` per path.
- `opAddFixed(Strings, Strings)` (static) — `--add-fixed`, with `--recursive` toggle; first arg is the hash algorithm.
- `opPrintFixedPath(Strings, Strings)` (static) — `--print-fixed-path` (cache helper for `nix-prefetch-url`); takes hash-algo + hash + name.
- `maybeUseOutputs(const StorePath&, bool useOutput, bool forceRealise)` (static, returns `StorePathSet`) — expands a derivation to its outputs when `--use-output/-u` is set; throws on floating CA derivations whose outputs are unknown.
- `printTree(const StorePath&, const std::string& firstPad, const std::string& tailPad, StorePathSet & done)` (static) — text dependency tree using `treeLast/treeConn/treeNull/treeLine` constants; topologically sorts.
- `opQuery(Strings, Strings)` (static) — `--query/-q` master handler dispatching on `QueryType`; supports `--outputs`, `--requisites/-R`, `--references`, `--referrers/--referers`, `--referrers-closure/--referers-closure`, `--deriver/-d`, `--valid-derivers`, `--binding/-b`, `--hash`, `--size`, `--tree`, `--graph`, `--graphml`, `--resolve`, `--roots`, plus `--use-output/-u`, `--force-realise/--force-realize/-f`, `--include-outputs`.
- `opPrintEnv(Strings, Strings)` (static) — `--print-env`; emits shell-sourceable `export VAR; VAR=...` lines for the derivation's env plus `_args`.
- `opReadLog(Strings, Strings)` (static) — `--read-log/-l`; needs a `LogStore`.
- `opDumpDB(Strings, Strings)` (static) — `--dump-db`; iterates either `opArgs` or all valid paths.
- `registerValidity(bool reregister, bool hashGiven, bool canonicalise)` (static) — used by `--load-db` and `--register-validity`; reads `decodeValidPathInfo` records from stdin and calls `LocalStore::registerValidPaths`.
- `opLoadDB(Strings, Strings)` (static) — `--load-db`.
- `opRegisterValidity(Strings, Strings)` (static) — `--register-validity` (`--reregister`, `--hash-given`).
- `opCheckValidity(Strings, Strings)` (static) — `--check-validity` (`--print-invalid`).
- `opGC(Strings, Strings)` (static) — `--gc` (`--print-roots`, `--print-live`, `--print-dead`, `--max-freed`); honours mutually-exclusive option combinations.
- `opDelete(Strings, Strings)` (static) — `--delete` (`--ignore-liveness`).
- `opDump(Strings, Strings)` (static) — `--dump`; calls `dumpPath` to stdout.
- `opRestore(Strings, Strings)` (static) — `--restore`; calls `restorePath` from stdin.
- `opExport(Strings, Strings)` (static) — `--export`; calls `exportPaths` to stdout.
- `opImport(Strings, Strings)` (static) — `--import`; calls `importPaths(NoCheckSigs)` from stdin.
- `opInit(Strings, Strings)` (static) — `--init` (currently a no-op; comment notes tables are auto-initialised).
- `opVerify(Strings, Strings)` (static) — `--verify` (`--check-contents`, `--repair`); calls `Store::verifyStore`; throws `Exit(1)` if fixes remain.
- `opVerifyPath(Strings, Strings)` (static) — `--verify-path`; throws `Exit(status)` with bitmask.
- `opRepairPath(Strings, Strings)` (static) — `--repair-path`; calls `Store::repairPath` per arg.
- `opOptimise(Strings, Strings)` (static) — `--optimise`/`--optimize`; calls `optimiseStore`.
- `opServe(Strings, Strings)` (static) — `--serve` (`--write`); implements the legacy `ServeProto` over stdin/stdout (`QueryValidPaths`, `QueryPathInfos`, `DumpStorePath`, `ImportPaths`, `BuildPaths`, `BuildDerivation`, `QueryClosure`, `AddToStoreNar`); inner lambda `getBuildSettings` reads `BuildOptions` honouring protocol-version-specific fields (`maxLogSize` ≥ 2.2, `runDiffHook` ≥ 2.3, `keepFailed` ≥ 2.7).
- `opGenerateBinaryCacheKey(Strings, Strings)` (static) — `--generate-binary-cache-key` (writes secret 0600 + public 0666 key files via `writeFile` with `FsSync::Yes`).
- `opVersion(Strings, Strings)` (static) — `--version`.
- `main_nix_store(int argc, char ** argv)` (static, returns `int`) — argument parser via `parseCmdLine` dispatching to all `op*` handlers; supports `--add-root`, `--stdin` (slurps newline-separated args from cin), `--indirect` (no-op), `--no-output`, with hacks pushing `--max-freed`/`--max-links`/`--max-atime` arguments through `opFlags`; opens the store unless the operation is `opDump`/`opRestore`.

### Command / legacy registrations
- `static RegisterLegacyCommand r_nix_store("nix-store", main_nix_store);`.

### Operation handler table (25 entries)
| Flag(s) | Handler |
|---|---|
| `--realise`, `--realize`, `-r` | `opRealise` |
| `--add`, `-A` | `opAdd` |
| `--add-fixed` | `opAddFixed` |
| `--print-fixed-path` | `opPrintFixedPath` |
| `--delete` | `opDelete` |
| `--query`, `-q` | `opQuery` |
| `--print-env` | `opPrintEnv` |
| `--read-log`, `-l` | `opReadLog` |
| `--dump-db` | `opDumpDB` |
| `--load-db` | `opLoadDB` |
| `--register-validity` | `opRegisterValidity` |
| `--check-validity` | `opCheckValidity` |
| `--gc` | `opGC` |
| `--dump` | `opDump` |
| `--restore` | `opRestore` |
| `--export` | `opExport` |
| `--import` | `opImport` |
| `--init` | `opInit` |
| `--verify` | `opVerify` |
| `--verify-path` | `opVerifyPath` |
| `--repair-path` | `opRepairPath` |
| `--optimise`, `--optimize` | `opOptimise` |
| `--serve` | `opServe` |
| `--generate-binary-cache-key` | `opGenerateBinaryCacheKey` |
| `--version` | `opVersion` |

### Type aliases
- `typedef void (*Operation)(Strings opFlags, Strings opArgs);` — operation pointer (different signature from nix-env's; no `Globals`).

### Macros / globals
- `static std::filesystem::path gcRoot;` — `--add-root` target.
- `static int rootNr = 0;` — root link counter (parallels nix-instantiate.cc).
- `static bool noOutput = false;` — `--no-output`.
- `static std::shared_ptr<Store> store;` — process-wide store handle (not opened for `--dump`/`--restore`).

---

## File: src/nix/unix/daemon.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `AuthorizationSettings` (struct, derives `Config`) — daemon-only auth settings: `trustedUsers` (defaults `{root}`) and `allowedUsers` (defaults `{*}`).
- `StdIO` (struct) — empty tag for the std-io daemon mode.
- `CmdDaemon` (struct, derives `StoreConfigCommand`) — modern `nix daemon` subcommand wrapping the same `runDaemon` logic; carries `stdio`, `isTrustedOpt`, `processOps`, `socketPath` members.

### Functions
- `splice(int fd_in, void* off_in, int fd_out, void* off_out, size_t len, unsigned int flags)` (static, non-Linux only) — userland fallback for the Linux `splice(2)` syscall using an 8192-byte bounce buffer; ignores most flags.
- `sigChldHandler(int sigNo)` (static) — saves/restores `errno`, writes a single byte to `sigChldPipe.pipe.writeSide`; aborts on hard write failure.
- `setSigChldAction(bool autoReap)` (static) — installs or removes `sigChldHandler` via `sigaction(SIGCHLD, ...)`.
- `matchUser(std::string_view user, const struct group & gr)` (static) — true if user is in the group's `gr_mem`.
- `matchUser(const std::optional<std::string>& user, const std::optional<std::string>& group, const Strings & users)` (static, overload) — matches against the `trustedUsers`/`allowedUsers` list, including `@group` syntax (looking up via `getgrnam`) and `*` wildcard.
- `authPeer(const unix::PeerInfo&)` (static, returns `std::pair<TrustedFlag, std::optional<std::string>>`) — looks up `pwuid`/`grgid`, returns `(TrustedFlag, optional<username>)`; throws if unauthorised; rejects users in the `buildUsersGroup`.
- `daemonLoop(ref<const StoreConfig>, std::optional<TrustedFlag>, std::filesystem::path socketPath)` (static) — `chdir("/")`, creates `sigChldPipe`, sets up cgroup if `useCgroups` (Linux only, requiring `Xp::Cgroups`), opens the socket via `unix::serveUnixSocket` with `socketMode=0666` and `activationName="nix-daemon.socket"`, reaps children via `waitpid`+`WNOHANG` on the auxiliary self-pipe, throws `unix::AbortServeSocket` once `crashCount` (incremented for fatal signals like SIGILL/SIGSEGV/SIGBUS/SIGABRT/SIGSYS/SIGFPE) reaches `crashLimit = 64`, forks per-connection workers via `startProcess` that call `processConnection`.
- `forwardStdioConnection(RemoteStore&)` (static) — `select(2)` loop over the daemon socket and stdin, splicing data both directions via `splice` with `SSIZE_MAX` length.
- `processStdioConnection(ref<Store>, TrustedFlag)` (static) — runs `processConnection` over stdin/stdout in `daemon::NotRecursive` mode.
- `runDaemon(ref<StoreConfig>, DaemonMode, std::optional<TrustedFlag>, bool processOps)` (static) — clears `pathInfoCacheSize`, visits the `DaemonMode` variant; for `StdIO` opens the store, sets `processOps |= !forceTrustClientOpt || *forceTrustClientOpt != NotTrusted`, then either forwards (when downcastable to `RemoteStore`) or processes; for `UnixSocket` resolves the socket path (defaulting via `getDaemonSocketPath`), warns on collisions with the store's own socket, and calls `daemonLoop`.
- `main_nix_daemon(int argc, char ** argv)` (static, returns `int`) — legacy entry; parses `--daemon` (ignored), `--stdio`, `--force-trusted`/`--force-untrusted`/`--default-trust` (gated on `Xp::DaemonTrustOverride`), `--process-ops` (gated on `Xp::MountedSSHStore`), then calls `runDaemon` over the store from `settings.storeUri`.
- `CmdDaemon::CmdDaemon()` — registers `--stdio`, `--force-trusted`, `--force-untrusted`, `--default-trust` (each gated on `Xp::DaemonTrustOverride`), `--socket-path` (label `"path"`), `--process-ops` (gated on `Xp::MountedSSHStore`).
- `CmdDaemon::description()` — "daemon to perform store operations on behalf of non-root clients".
- `CmdDaemon::category()` — returns `catUtility`.
- `CmdDaemon::doc()` — embeds `daemon.md`.
- `CmdDaemon::run(ref<StoreConfig>)` — rejects `--stdio` combined with `--socket-path`, then dispatches to `runDaemon`.

### Command / legacy registrations
- `static GlobalConfig::Register rAuthorizationSettings(&authorizationSettings);`.
- `static RegisterLegacyCommand r_nix_daemon("nix-daemon", main_nix_daemon);`.
- `static auto rCmdDaemon = registerCommand2<CmdDaemon>({"daemon"});`.

### Type aliases
- `using UnixSocket = std::optional<std::filesystem::path>;` — daemon socket path (or default).
- `using DaemonMode = std::variant<StdIO, UnixSocket>;` — daemon connection mode.

### Macros / globals
- `AuthorizationSettings authorizationSettings;` — global instance.
- `static unix::SelfPipe sigChldPipe;` — self-pipe for SIGCHLD wakeups.
- `#define SPLICE_F_MOVE 0` (non-Linux) — keep client conformance even though `splice` is emulated.
- `static constexpr unsigned crashLimit = 64;` — daemon worker crash cap (defined inside `daemonLoop`).
- `///@file` doc marker at the top of the file.

---

## File: src/nix/unix/store-roots-daemon.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `CmdRootsDaemon` (struct, derives `StoreConfigCommand`) — implements `nix store roots-daemon`; serves runtime GC roots over a Unix socket.

### Functions
- `CmdRootsDaemon::CmdRootsDaemon()` — default-constructed (empty body).
- `CmdRootsDaemon::description()` — "run a daemon that returns garbage collector roots on request".
- `CmdRootsDaemon::doc()` — embeds `store-roots-daemon.md`.
- `CmdRootsDaemon::experimentalFeature()` — returns `Xp::LocalOverlayStore`.
- `CmdRootsDaemon::run(ref<StoreConfig>)` — downcasts to `LocalStoreConfig` (throws `UsageError` otherwise), opens the local store's roots socket via `unix::serveUnixSocket` (`socketMode=0666`, `activationName="nix-roots-daemon.socket"`), then on each accept spawns a detached `std::thread` that writes `findRuntimeRootsUnchecked` results to an `FdSink` as NUL-terminated paths and closes the connection.

### Command / legacy registrations
- `static auto rCmdStoreRootsDaemon = registerCommand2<CmdRootsDaemon>({"store", "roots-daemon"});`.

### Type aliases
- (none)

### Macros / globals
- (none)

---

# Cross-file observations

## Modern `nix store ...` vs legacy `nix-store`

The new CLI splits each operation flag of `nix-store` into its own modern `Cmd*` subclass. Concrete duplicate pairs:

| Legacy handler (`nix-store.cc`) | Modern command class | Modern file |
|---|---|---|
| `opRealise` (`--realise/-r`) | (closest equivalent is `nix build`, not in this shard) | n/a |
| `opAdd` (`--add`) | `CmdAdd`/`CmdAddFile`/`CmdAddPath` | `add-to-store.cc` |
| `opAddFixed` (`--add-fixed`) | (subsumed by `nix store add --hash-algo` plus `--mode flat`) | `add-to-store.cc` |
| `opPrintFixedPath` | (no modern equivalent in this shard) | n/a |
| `opQuery` (`--query`) | (split across `nix path-info`/`nix why-depends`/etc., not in this shard) | n/a |
| `opGC` (`--gc`) | `CmdStoreGC` | `store-gc.cc` |
| `opDelete` (`--delete`) | `CmdStoreDelete` | `store-delete.cc` |
| `opDump`/`opRestore` (`--dump`/`--restore`) | `CmdDumpPath` (`store dump-path`) and `CmdDumpPath2` (`nar pack`) | `dump-path.cc` |
| `opExport`/`opImport` | (no current equivalent in this shard) | n/a |
| `opVerify` (`--verify`) | `CmdVerify` (`store verify`) covers per-path NAR-hash check | `verify.cc` |
| `opVerifyPath` | `CmdVerify` covers per-path NAR-hash check | `verify.cc` |
| `opRepairPath` (`--repair-path`) | `CmdStoreRepair` (`store repair`) | `store-repair.cc` |
| `opOptimise` (`--optimise`) | `CmdOptimiseStore` (`store optimise`) | `optimise-store.cc` |
| `opGenerateBinaryCacheKey` | `CmdKeyGenerateSecret` + `CmdKeyConvertSecretToPublic` (`nix key ...`) | `sigs.cc` |
| `opPrintEnv`, `opReadLog`, `opDumpDB`, `opLoadDB`, `opRegisterValidity`, `opCheckValidity`, `opServe`, `opInit` | (no equivalents in this shard) | n/a |

Despite different framing, both code paths reach the same store APIs — `Store::optimiseStore()`, `Store::repairPath()`, `GcStore::collectGarbage()`, `Store::buildPaths()`. The legacy CLI duplicates the GC dispatch logic three times: in `nix-store.cc opGC`, `nix-collect-garbage.cc main_nix_collect_garbage`, and `store-gc.cc CmdStoreGC::run`. All three open a `GcStore`, set `pathsToDelete = GCOptions::WholeStore{}`, and wrap the `collectGarbage` call in a `Finally`. They differ in which results they print: `store-gc.cc` and `nix-collect-garbage.cc` always call `printFreed`; `nix-store.cc opGC` instead prints `results.paths` line-by-line when the action is `gcReturnDead`/`gcReturnLive`/`--print-roots` and only calls `printFreed` for `gcDeleteDead`. `nix-collect-garbage` additionally walks profile directories (XDG and legacy locations) to delete old generations before invoking the GC.

NAR streaming has three nearly identical entry points: `CmdDumpPath::run` (`store dump-path`), `CmdDumpPath2::run` (`nar pack`), and `nix-store.cc opDump`. The first two route through `dump-path.cc:getNarSink()`, which throws `UsageError` if stdout is a TTY; `nix-store.cc opDump` constructs the `FdSink` directly and skips that check. All three then call `narFromPath` or `dumpPath` and flush.

## Mixin / scaffolding patterns

- `MixCat` and `MixLs` are reused across both `store {cat,ls}` and `nar {cat,ls}` (i.e., `cat.cc` and `ls.cc`). The store and NAR variants only differ in their `run()` source-accessor construction: `CmdCatStore`/`CmdLsStore` resolve the path via `store->toStorePath` + `requireStoreObjectAccessor`, while `CmdCatNar` parses the NAR with a custom `NullFileSystemObjectSink` and `CmdLsNar` builds a `makeLazyNarAccessor`.
- Many commands derive from `StorePathsCommand`, `StorePathCommand`, `CopyCommand`, `BuiltPathsCommand`, `InstallablesCommand`, often combined with `MixJSON`, `MixDryRun`, `MixProfile`, `MixNoCheckSigs`. These mixins consistently come from `nix/cmd/command.hh`.
- `NixMultiCommand` is reused in `CmdStore`, `CmdNar`, and `CmdKey`. The first two enumerate children dynamically with `RegisterCommand::getCommandsFor({"store"})`/`({"nar"})`; `CmdKey` hand-rolls the child list with explicit `make_ref<...>` factories.
- `StoreConfigCommand` is the modern base for daemon entry points (`CmdDaemon` and `CmdRootsDaemon`).

## Legacy-tool boilerplate

The legacy entry points share a recurring preamble: a `parseCmdLine`/`MyArgs`-style argument loop and a final `RegisterLegacyCommand` line. `nix-build.cc`, `nix-instantiate.cc`, and `nix-env.cc` each define an inner `struct MyArgs : LegacyArgs, MixEvalArgs`. `nix-env.cc` and `nix-store.cc` additionally define a function-pointer `Operation` typedef and a flag-to-handler dispatch table — a clear opportunity for a shared "subcommand-by-flag" helper. They differ in that `nix-env`'s `Operation` carries a `Globals&`; `nix-store`'s does not (it uses a file-scope `static std::shared_ptr<Store> store`). The 12-entry `nix-env` and 25-entry `nix-store` dispatch tables are confirmed exact above.

`nix-channel.cc`, `nix-copy-closure.cc`, and `nix-collect-garbage.cc` have a flatter shape: just a single `parseCmdLine` over a manual command-discriminator enum or a handful of bools (no `MyArgs`, no `Operation`).

`build-remote.cc` is a uniquely shaped legacy entry: it doesn't use `parseCmdLine` at all — it reads its single `verbosity` argument from `argv[1]` and the rest of its protocol from stdin, switching the logger to JSON for the parent process.

## Daemon entry points

`unix/daemon.cc` deliberately exposes both a legacy `nix-daemon` and a modern `nix daemon` over the same `runDaemon` core, threading the same `DaemonMode = std::variant<StdIO, UnixSocket>` variant through both. Authentication is centralised in `authPeer`/`matchUser` (only used by the unix-socket path). `unix/store-roots-daemon.cc` is modern-only and gated behind `Xp::LocalOverlayStore`.

## Reusable graph helpers

`nix-store/dotgraph.{cc,hh}` and `nix-store/graphml.{cc,hh}` expose a single `printDotGraph` / `printGraphML` function each; both walk the references closure with the same BFS pattern (`StorePathSet workList`, `StorePathSet doneSet`, `extract(begin())`). Could share a closure-walk helper. Note that `dotgraph.cc` reverses the edge direction (`p -> path`) while `graphml.cc` keeps it (`path -> p`).

## Globals reuse

The pair `(std::filesystem::path gcRoot, int rootNr)` appears in both `nix-store/nix-store.cc` (file scope, both `static`) and `nix-instantiate/nix-instantiate.cc` (`gcRoot` external linkage, `rootNr` `static`). Both increment `rootNr` when constructing GC root link names like `result-N`.
