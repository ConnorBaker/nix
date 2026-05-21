# Nix C++ codebase inventory — top-level index

This is a per-shard inventory of every named declaration (namespaces, classes,
free functions, member functions, enums, type aliases, macros, globals) in the
non-test C++ source under `src/` (628 files, ~138K lines). Each shard was first
catalogued by a Haiku subagent and then verified by an Opus subagent against the
source. The goal is to surface duplication and refactoring opportunities; this
document is a navigation map into the per-shard verified inventories below. For
the body content of any shard, follow the link in the table.

## Shard table

| Shard | Title | Scope | Files | Verified doc |
| ----- | ----- | ----- | ----- | ------------ |
| 01 | libutil — IO | Path/serialise/file-system/source-accessors/NAR/tar/file-CA | 40 | [verified/01-libutil-io.md](verified/01-libutil-io.md) |
| 02 | libutil — data | Hashing, base-N encodings, URL/git/JSON/XML, signature, compression | 33 | [verified/02-libutil-data.md](verified/02-libutil-data.md) |
| 03 | libutil — runtime | Configuration, error/logging, args, processes, threading, signals, env, XDG | 39 | [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md) |
| 04 | libutil — misc / platform | Header-only utilities + per-platform unix/linux/freebsd/windows impls | 79 | [verified/04-libutil-misc.md](verified/04-libutil-misc.md) |
| 05 | libstore — core types | Store/StoreConfig hierarchy, StorePath, ContentAddress, DerivedPath, BuildResult | 41 | [verified/05-libstore-core.md](verified/05-libstore-core.md) |
| 06 | libstore — derivations | Derivation parse/serialise/options, names, profiles, machines, sqlite, length-prefix proto helper | 26 | [verified/06-libstore-derivations.md](verified/06-libstore-derivations.md) |
| 07 | libstore — local stores | LocalStore, LocalOverlayStore, RestrictedStore, DummyStore, GC, sqlite, settings/globals, schema | 22 | [verified/07-libstore-local.md](verified/07-libstore-local.md) |
| 08 | libstore — remote stores | RemoteStore + UDS/SSH/Legacy-SSH/Mounted-SSH, BinaryCacheStore + Local/HTTP/S3, AWS auth, file-transfer | 36 | [verified/08-libstore-remote.md](verified/08-libstore-remote.md) |
| 09 | libstore — protocols | Worker/Serve/Common protocols, length-prefixed serialiser helpers, daemon dispatch | 16 | [verified/09-libstore-protocol.md](verified/09-libstore-protocol.md) |
| 10 | libstore — build | Goal hierarchy, Worker, DerivationBuilder per-platform, build-log, hooks, builtin builders | 47 | [verified/10-libstore-build.md](verified/10-libstore-build.md) |
| 11 | libexpr — eval | EvalState, Value, Bindings/AttrSet, eval-cache, GC, profiler, settings, function-trace | 31 | [verified/11-libexpr-eval.md](verified/11-libexpr-eval.md) |
| 12 | libexpr — parse | Lexer/parser, AST (`Expr`), printers (text/JSON/XML/ambiguous), `get-drvs`, search-path, JSON↔Value | 23 | [verified/12-libexpr-parse.md](verified/12-libexpr-parse.md) |
| 13 | libexpr — primops | Built-in operations (arithmetic, attrset, list, string, file, fetcher) | 8 | [verified/13-libexpr-primops.md](verified/13-libexpr-primops.md) |
| 14 | libfetchers | InputScheme hierarchy, attrs/cache/registry, git/mercurial/github/tarball, git-utils, LFS | 27 | [verified/14-libfetchers.md](verified/14-libfetchers.md) |
| 15 | libflake + libmain | Flake / FlakeRef / lockfile / settings; common-args, loggers, plugin, progress-bar, shared, unix/stack | 24 | [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md) |
| 16 | libcmd | Command/Installable hierarchies, common eval args, REPL, markdown, network-proxy, unix-socket-server | 36 | [verified/16-libcmd.md](verified/16-libcmd.md) |
| 17 | nix CLI — modern (1) | `nix` subcommands: build/eval/repl/search/develop/run/bundle/flake/registry/profile/path-info/edit/log/hash/upgrade-nix | 38 | [verified/17-nix-modern-1.md](verified/17-nix-modern-1.md) |
| 18 | nix CLI — modern (2) + legacy | `nix store/nar/key/{add,verify,sigs,copy,...}`, daemon, plus `nix-build`, `nix-env`, `nix-store`, `nix-channel`, etc. | 32 | [verified/18-nix-modern-2-legacy.md](verified/18-nix-modern-2-legacy.md) |
| 19 | C bindings + misc | libutil-c / libstore-c / libexpr-c / libfetchers-c / libflake-c / libmain-c, plus clang-tidy plugin and nswrapper | 12 | [verified/19-c-bindings-misc.md](verified/19-c-bindings-misc.md) |

## Subsystem map

The 19 shards are grouped by the library or component they cover. Larger
libraries are split across several shards; smaller libraries (libcmd, libflake,
libmain, libfetchers) get one shard each.

### libutil — foundation (shards 01-04)

The lowest layer: pure utility code with no Nix-specific concepts, used by
every other library. The four shards split libutil along axes of subject
matter rather than by directory.

- **[shard 01](verified/01-libutil-io.md)** — IO. Source/Sink/BufferedSink/FdSink, `serialise.hh`'s adapter zoo, the `SourceAccessor` hierarchy (POSIX, Windows, mounted, union, caching, memory), NAR archive read/write (`archive.{cc,hh}`), the `FileSystemObjectSink` family (`fs-sink`, `MemorySink`, `RestoreSink`), tarfile (libarchive) and file-content-address selection.
- **[shard 02](verified/02-libutil-data.md)** — Data and encoding. `Hash`/`HashAlgorithm`/`HashFormat`, `base16`/`base64`/`BaseNix32`, URL parser (`ParsedURL`/`VerbatimURL`), git tree/blob ATerm, JSON helpers (`json-utils`/`json-impls`/`json-non-null`), XML writer, signing (`Signer`/`LocalSigner`/`PublicKey`/`SecretKey`), compression (`makeCompressionSink`/`makeDecompressionSink` over zstd/brotli/xz/...).
- **[shard 03](verified/03-libutil-runtime.md)** — Runtime. `Config`/`AbstractSetting`/`BaseSetting<T>`/`GlobalConfig`, the `Args`/`MultiCommand`/`Command` machinery, errors (`BaseError`/`SystemError`/`SysError`/`MakeError`), logging (`Logger`/`SimpleLogger`/`JSONLogger`/`TeeLogger`), processes (`Pid`/`startProcess`/`runProgram`), threading (`ThreadPool`, `Sync<T>`/`SharedSync<T>`, async coroutines via `nix::asio`), signals/interrupts, executable path lookup, environment vars, XDG dirs, experimental features, unix domain sockets.
- **[shard 04](verified/04-libutil-misc.md)** — Misc and platform. The header-only utilities (`Finally`, `fmt`/`HintFmt`, `fun<T>`, `ref<T>`, `Pool<R>`, `LRUCache`, `ChunkedVector`, `MaintainCount`, `Checked` arithmetic, sort, topo-sort, closures, `BumpMemoryResource`, terminal/wcwidth) plus *every* platform-specific implementation under `unix/`, `linux/`, `freebsd/`, `windows/` (file descriptors, file system, processes, signals, env vars, XDG/known folders, namespaces/cgroups, jails, async pipes).

### libstore — core types (shards 05-06)

Identifier types, content addressing, derivation representation, build-result
types, plus the `Store` interface used by every concrete store implementation.

- **[shard 05](verified/05-libstore-core.md)** — Core types. `Store`/`StoreConfig`/`StoreDirConfig` (the abstract base), `StoreReference`/`StoreFactory`/`Implementations` (URI scheme registry), `StorePath`, `path-info` (`UnkeyedValidPathInfo`, `ValidPathInfo`, signing/fingerprint), references scanning/rewriting, `StorePathWithOutputs`, `ContentAddress`/`ContentAddressMethod`/`StoreReferences`, `DerivedPath`/`SingleDerivedPath` and `DerivedPathMap<V>`, downstream placeholders, `OutputsSpec`/`ExtendedOutputsSpec`, outputs-query/realisation, `make-content-addressed`, posix-fs canonicalise, `BuildResult`/`KeyedBuildResult`/`BuildError`.
- **[shard 06](verified/06-libstore-derivations.md)** — Derivations and surrounding helpers. Derivation parsing/unparsing (ATerm + JSON), `DerivationOutput`/`DerivationType`, `DerivationOptions<Input>` (per-output checks), `parsed-derivations` (StructuredAttrs), drv name comparison (`names.cc`), profiles + generations, signing keys facade, log-store mix-in, misc store helpers (closures, missing paths, derived-path resolution), optimise-store, pathlocks, machine list, length-prefixed proto helper template.

### libstore — store implementations (shards 07-08)

Concrete `Store` subclasses, split by deployment locality.

- **[shard 07](verified/07-libstore-local.md)** — Local stores. `LocalStore` (the SQLite-backed reference implementation), `LocalFSStore` (filesystem accessor mix-in), `LocalOverlayStore` (lower/upper layered stores), `IndirectRootStore`, `GcStore`, `DummyStore` (in-memory), `RestrictedStore` (recursive-nix sandbox view), GC machinery (`gc.cc`, `local-gc.cc`), `pathlocks` (unix + windows), per-build user lock (`SimpleUserLock`/`AutoUserLock`), the SQLite wrapper, the SQL schema (`schema.sql`, `ca-specific-schema.sql`), `LocalSettings`/`GCSettings`/`AutoAllocateUidSettings`, the global `Settings`.
- **[shard 08](verified/08-libstore-remote.md)** — Remote stores. `RemoteStore` (worker-protocol pool with a `Pool<Connection>`), `UDSRemoteStore`, `BinaryCacheStore` (abstract) + `LocalBinaryCacheStore`/`HttpBinaryCacheStore`/`S3BinaryCacheStore`, S3 URL parsing + AWS credential providers, `SSHMaster`, `SSHStore` and `MountedSSHStore` (worker-proto over SSH) plus `LegacySSHStore` (serve-proto over SSH), the `RemoteFSAccessor`, the NAR-info disk cache (SQLite at `binary-cache-v8.sqlite`), the curl-based `FileTransfer` and `FileTransferRequest`/`FileTransferResult`, the export/import wire format.

### libstore — wire protocols and daemon (shard 09)

- **[shard 09](verified/09-libstore-protocol.md)** — `WorkerProto`/`ServeProto`/`CommonProto` (three parallel namespace-shaped struct types), version negotiation (`WorkerProto::Version` with `FeatureSet` partial ordering vs `ServeProto::Version` with total ordering), per-protocol `Serialise<T>` template specialisations, `BasicConnection`/`BasicClientConnection`/`BasicServerConnection`, the daemon's `performOp` switch handling 37 worker-protocol opcodes.

### libstore — build infrastructure (shard 10)

- **[shard 10](verified/10-libstore-build.md)** — The build system. `Goal` hierarchy (`DerivationTrampolineGoal` → `DerivationGoal` → `DerivationResolutionGoal` / `DerivationBuildingGoal` / `PathSubstitutionGoal` / `DrvOutputSubstitutionGoal`) with C++20 coroutines via `Goal::Co`/`promise_type`, the `Worker` coordinator with goal caches and cross-thread waker, the `DerivationBuilder` interface and `DerivationBuilderImpl` per-platform diamond (Linux user-namespace + cgroup + pivot-root, FreeBSD jail + nullfs + devfs, Darwin sandbox-init + Rosetta posix_spawn, plus `ExternalDerivationBuilder` driven by `Xp::ExternalBuilders`), `HookInstance` for the build-hook protocol, builtin builders (`buildenv`, `fetchurl`, `unpack-channel`), build-log line buffering with JSON activity parsing.

### libexpr — Nix language (shards 11-13)

The Nix expression language itself: runtime, parsing, builtins.

- **[shard 11](verified/11-libexpr-eval.md)** — Eval runtime. `EvalState`, `Value` (the discriminated union, with two storage layouts: a generic 24-byte form and an x86_64 16-byte bit-packed form using SSE2 atomic loads), `Bindings`/`AttrSet` (with the layered "//" optimisation and chunked symbol table), eval cache (SQLite-backed memoisation of attr-path traversal), profiler, eval errors (`EvalError` cacheable vs `EvalBaseError` not), settings, function-trace, position table, GC integration with Boehm.
- **[shard 12](verified/12-libexpr-parse.md)** — Parser, AST, printers. `lexer.l` (flex), `parser.y` (bison), the `Expr` AST hierarchy in `nixexpr.{cc,hh}` (subclasses for every syntactic form), four parallel value renderers (`Printer`, `printAmbiguous`, `printValueAsJSON`, `printValueAsXML`), `get-drvs.cc` (DrvInfo extraction), JSON↔Value (`json-to-value`, `value-to-json`), search path, paths.
- **[shard 13](verified/13-libexpr-primops.md)** — Primops. The 8 primop source files defining the >100 entries of `builtins`. `primops.cc` is the bulk of them; `context.cc` handles string-context primops; `fetchClosure.cc`, `fetchMercurial.cc`, `fetchTree.cc`, `fromTOML.cc` are the per-fetcher primops. The `RegisterPrimOp` static-registration pattern is universal here.

### libfetchers / libflake / libmain / libcmd (shards 14-16)

Higher-level orchestration sitting between libexpr and the CLI.

- **[shard 14](verified/14-libfetchers.md)** — libfetchers. The `InputScheme` hierarchy (8 concrete schemes: `IndirectInputScheme`, `PathInputScheme`, `GitInputScheme`, `MercurialInputScheme`, plus `GitArchiveInputScheme` → `GitHubInputScheme`/`GitLabInputScheme`/`SourceHutInputScheme`, plus `CurlInputScheme` → `FileInputScheme`/`TarballInputScheme`), the `Cache` (SQLite at `fetcher-cache-v4.sqlite`) and `InputCache` (in-memory), `git-utils` wrapping libgit2, `git-lfs-fetch`, fetch-to-store, the registry, filtering source accessors, fetch-settings.
- **[shard 15](verified/15-libflake-libmain.md)** — libflake + libmain. libflake: flake parsing/locking (`Flake`/`LockedFlake`/`LockFile`), `FlakeRef`, flake settings, flake primops, URL-name extraction. libmain: `MixCommonArgs`, log format dispatch (`makeProgressBarLogger`/`makeJSONLogger`/`makeSimpleLogger`), plugin loading, progress-bar implementation, `handleExceptions` shared entry, stack-overflow handling on Unix.
- **[shard 16](verified/16-libcmd.md)** — libcmd. The `Command` mixin tower (`Command` → `StoreConfigCommand` → `StoreCommand` → `EvalCommand` → `MixFlakeOptions` → `SourceExprCommand` → `RawInstallablesCommand` → `InstallablesCommand` → `BuiltPathsCommand` → `StorePathsCommand` → `StorePathCommand`, plus `InstallableCommand` → `InstallableValueCommand`, plus mixins like `MixEvalArgs`/`MixProfile`/`MixOutLinkBase`), the `Installable` hierarchy (`Installable` → `InstallableValue` → `InstallableAttrPath`/`InstallableFlake`, plus `InstallableDerivedPath`), `BuiltPath`/`SingleBuiltPath`, the REPL (`NixRepl`/`ReplInteracter`), markdown rendering (lowdown), built-log retrieval, editor invocation, network-proxy environment, the `RegisterCommand`/`RegisterLegacyCommand` registries.

### Modern `nix` CLI and legacy `nix-*` tools (shards 17-18)

- **[shard 17](verified/17-nix-modern-1.md)** — Modern `nix` subcommands part 1: `nix build`, `nix eval`, `nix repl`, `nix search`, `nix develop`/`nix shell`/`nix print-dev-env`, `nix run`, `nix bundle`, `nix env shell`, `nix flake` (sub-tree of init/check/show/lock/metadata/clone/archive/update/prefetch-inputs), `nix registry`, `nix profile`, `nix path-info`, `nix path-from-hash-part`, `nix store-prefetch-file`, `nix realisation`, `nix derivation` (show/add), `nix edit`, `nix config` (show/check), `nix diff-closures`, `nix why-depends`, `nix log`, `nix fmt`/`nix formatter`, `nix upgrade-nix`, `nix hash`, plus `main.cc`/crash-handler/man-pages/self-exe.
- **[shard 18](verified/18-nix-modern-2-legacy.md)** — Modern `nix` subcommands part 2 plus legacy tools: `nix store ...` (info/gc/delete/repair/copy-log/optimise/verify), `nix copy`, `nix cat`/`nix ls` (store + nar variants via `MixCat`/`MixLs`), `nix nar` (pack/cat/ls), `nix store dump-path`, `nix store add`/`nix store add-file`/`nix store add-path`, `nix store make-content-addressed`, `nix key generate-secret`/`convert-secret-to-public`, `nix store sign`, `nix store verify`, plus `nix daemon`/`nix store-roots-daemon` and the legacy `nix-build`, `nix-env`, `nix-store`, `nix-channel`, `nix-collect-garbage`, `nix-copy-closure`, `nix-instantiate`, `build-remote`, the `dotgraph`/`graphml` helpers.

### C bindings and miscellany (shard 19)

- **[shard 19](verified/19-c-bindings-misc.md)** — The C ABI. `libutil-c` (`nix_api_util.{cc,h}`), `libstore-c` (with sub-headers for derivation and store_path), `libexpr-c` (`nix_api_expr`/`nix_api_value`/`nix_api_external`), `libfetchers-c`, `libflake-c`, `libmain-c`. Each library has a `nix_<libname>_init` idempotent entry and a uniform error-reporting protocol via `NIXC_CATCH_ERRS{,_RES,_NULL}`. Plus the clang-tidy plugin (currently a stub) and `nswrapper` (Linux helper for `newuidmap`/`newgidmap` outside the user namespace).

## Cross-shard topic index

A clustered index across shards. For each topic, follow the listed shard(s) for
the full set of declarations.

### Source accessors / file system abstraction

The `SourceAccessor` base presents a read-only filesystem-shaped API used by
the evaluator, the store, and tools like `nix store dump-path`.

- The base type, the static factories (`makeFSSourceAccessor`, `makeUnionSourceAccessor`, `makeMountedSourceAccessor`, `makeCachingSourceAccessor`, `makeEmptySourceAccessor`, `makeMemorySourceAccessor`), and the POSIX/Windows/file/directory backends: [verified/01-libutil-io.md](verified/01-libutil-io.md).
- The store-side `LocalStoreAccessor` (filesystem rooted at `realStoreDir`): [verified/07-libstore-local.md](verified/07-libstore-local.md).
- The `RemoteFSAccessor` (per-path NAR fetch + on-disk NAR cache, used by both `RemoteStore` and `BinaryCacheStore`): [verified/08-libstore-remote.md](verified/08-libstore-remote.md).
- The fetchers' filtering source accessor (used by git submodules, dirty-workdir handling): [verified/14-libfetchers.md](verified/14-libfetchers.md).

### Hashing and content addressing

- `Hash`, `HashAlgorithm` (BLAKE3/MD5/SHA1/SHA256/SHA512), `HashFormat` (Base64/Nix32/Base16/SRI), `HashSink`, the `parse*Prefixed`/`parseSRI` family, `compressHash` (XOR-fold): [verified/02-libutil-data.md](verified/02-libutil-data.md).
- `FileSerialisationMethod` (Flat/NixArchive) and `FileIngestionMethod` (Flat/NixArchive/Git), the `dumpPath`/`restorePath`/`hashPath` helpers selecting between them: [verified/01-libutil-io.md](verified/01-libutil-io.md).
- `ContentAddress`, `ContentAddressMethod`, `ContentAddressWithReferences`, `TextInfo`/`FixedOutputInfo`, the store-path computation that hashes them (`makeFixedOutputPathFromCA`): [verified/05-libstore-core.md](verified/05-libstore-core.md).
- The `HashModuloSink` and `RewritingSink` used for self-reference-invariant hashing in `make-content-addressed`: [verified/05-libstore-core.md](verified/05-libstore-core.md).
- Git tree/blob hashing (`git::dumpHash`, `dumpTree`, `parseTree`): [verified/02-libutil-data.md](verified/02-libutil-data.md).

### Serialisation (NAR, JSON, XML, length-prefixed)

- NAR (`narVersionMagic1 = "nix-archive-1"`): `parseDump`, `dumpPath`, `restorePath`, `copyNAR`, plus the `FileSystemObjectSink`/`CreateRegularFileSink` interfaces consumed during NAR parse: [verified/01-libutil-io.md](verified/01-libutil-io.md).
- JSON adl_serializer specialisations are scattered throughout; the shared scaffolding (`JSON_IMPL`, `JSON_IMPL_INNER`, `json_avoids_null<T>` trait, `getString`/`getObject`/`getArray`/`getInteger`): [verified/02-libutil-data.md](verified/02-libutil-data.md).
- XML emitter (`XMLWriter`, `XMLOpenElement` RAII, `XMLAttrs`): [verified/02-libutil-data.md](verified/02-libutil-data.md).
- Length-prefixed framing helper (`LengthPrefixedProtoHelper<Inner, T>` for `vector`/`set`/`tuple`/`map`): [verified/06-libstore-derivations.md](verified/06-libstore-derivations.md).
- The three protocol-specific Serialise frameworks (`WorkerProto::Serialise<T>`, `ServeProto::Serialise<T>`, `CommonProto::Serialise<T>`) and their delegation to the length-prefix helper: [verified/09-libstore-protocol.md](verified/09-libstore-protocol.md).
- Export/import wire format (`exportMagic = NIXE`): [verified/08-libstore-remote.md](verified/08-libstore-remote.md).

### URL / flakeref parsing

- `ParsedURL` (RFC-3986), `ParsedURL::Authority`, `VerbatimURL`, `parseURL`/`parseURLRelative`, `tryParseScpStyle`, `pathToUrlPath`/`urlPathToPath`, the regex constants in `url-parts.hh`, `isValidSchemeName`: [verified/02-libutil-data.md](verified/02-libutil-data.md).
- `S3AddressingStyle` and `ParsedS3URL` (with `toHttpsUrl` style selection): [verified/08-libstore-remote.md](verified/08-libstore-remote.md).
- `FlakeRef::parse`/`fromAttrs`/`toAttrs`, `parsePathFlakeRefWithFragment`, `parseFlakeIdRef`: [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md).
- Per-input-scheme URL handling (each `InputScheme::inputFromURL` reimplements URL splitting): [verified/14-libfetchers.md](verified/14-libfetchers.md).
- `getNameFromURL` (the URL → flake-name extractor): [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md).

### Compression

- `CompressionAlgo` enum, `parseCompressionAlgo`/`showCompressionAlgo`, the X-macro list `NIX_FOR_EACH_COMPRESSION_ALGO`, the compression sink hierarchy (`NoneSink`/`BrotliCompressionSink`/`BrotliDecompressionSink`/`ArchiveCompressionSink`/`ZstdMultiFrameCompressionSink`), `makeCompressionSink`/`makeDecompressionSink`/`compress`/`decompress`: [verified/02-libutil-data.md](verified/02-libutil-data.md).
- The settings glue specialisations for `BaseSetting<CompressionAlgo>` and `BaseSetting<std::optional<CompressionAlgo>>`: [verified/02-libutil-data.md](verified/02-libutil-data.md).
- HTTP-binary-cache compression dispatch by file suffix (`getCompressionMethod` for narinfo/ls/log): [verified/08-libstore-remote.md](verified/08-libstore-remote.md).

### Settings / configuration system

- `AbstractConfig`/`Config`/`AbstractSetting`/`BaseSetting<T>`/`Setting<T>`/`GlobalConfig`/`GlobalConfig::Register`, the per-type `BaseSetting<T>::parse`/`to_string`/`appendOrSet`/`trait` machinery, `ExperimentalFeatureSettings`, `MissingExperimentalFeature`, the `NIX_DECLARE_CONFIG_SERIALISER` macro: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- The `experimental-features.{cc,hh}` table and `Xp = ExperimentalFeature` enum: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- The libstore globals `Settings`/`LocalSettings`/`GCSettings`/`AutoAllocateUidSettings`/`LogFileSettings`/`NarInfoDiskCacheSettings`, plus the `BaseSetting<SandboxMode>`/`BaseSetting<PathsInChroot>`/`BaseSetting<StoreReference>` specialisations: [verified/07-libstore-local.md](verified/07-libstore-local.md).
- `WorkerSettings` (build-side): [verified/09-libstore-protocol.md](verified/09-libstore-protocol.md).
- The flake `Settings` and the trusted-flake-setting whitelist in `ConfigFile::apply`: [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md).
- Eval settings (`eval-settings.{cc,hh}`, `eval-profiler-settings`): [verified/11-libexpr-eval.md](verified/11-libexpr-eval.md).
- Fetcher settings (`fetch-settings`, plus per-fetcher overrides): [verified/14-libfetchers.md](verified/14-libfetchers.md).

### Error and logging

- `BaseError`/`Error`/`UsageError`/`UnimplementedError`/`SystemError`/`SysError`/`WinError`/`ExecError`, `CloneableError<Derived,Base>`, `Trace`/`ErrorInfo`/`Pos`, the `MakeError(name, parent)` macro, `handleExceptions`, `panic`, `unreachable`: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- Logger types (`Logger`, `SimpleLogger`, `JSONLogger`, `TeeLogger`), `Activity`/`PushActivity`, the JSON activity tags (`actCopyPath`, `actBuilds`, etc.) and result tags (`resBuildLogLine`, etc.), `makeJSONLogger`/`makeTeeLogger`/`applyJSONLogger`, `parseJSONMessage`/`handleJSONLogMessage`: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- The progress-bar logger (libmain): [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md).
- `BuildLog` (build-log-line buffering): [verified/10-libstore-build.md](verified/10-libstore-build.md).
- The eval-side `EvalError`/`EvalBaseError`/`EvalErrorBuilder<T>`/`RecoverableEvalError`/`StackOverflowError`: [verified/11-libexpr-eval.md](verified/11-libexpr-eval.md).
- `BuildError` and the `BuildResult::Failure::Status` enum: [verified/05-libstore-core.md](verified/05-libstore-core.md).

### Process spawning and sandboxing

- The `Pid`/`startProcess`/`runProgram`/`runProgram2`/`statusToString`/`statusOk`/`execvpe`/`killUser`/`ProcessOptions`/`RunOptions`/`ExecError` API: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md), with platform implementations in [verified/04-libutil-misc.md](verified/04-libutil-misc.md).
- The Linux `personality.{cc,hh}` helper, `linux-namespaces` (user/mount/pid namespaces), `cgroup` setup: [verified/04-libutil-misc.md](verified/04-libutil-misc.md).
- The FreeBSD jail wrapper (`AutoRemoveJail`): [verified/04-libutil-misc.md](verified/04-libutil-misc.md).
- The Linux Windows `nswrapper` helper that runs `newuidmap`/`newgidmap`: [verified/19-c-bindings-misc.md](verified/19-c-bindings-misc.md).
- Sandbox setup (chroot creation, bind-mounts, namespaces) in the `DerivationBuilder` family: [verified/10-libstore-build.md](verified/10-libstore-build.md).
- Recursive-Nix daemon plumbing in `DerivationBuilderImpl::startDaemon`/`stopDaemon`: [verified/10-libstore-build.md](verified/10-libstore-build.md).

### Threading primitives (Sync, Pool, ThreadPool, async)

- `Sync<T>`/`SharedSync<T>` mutex wrapper with `WriteLock`/`ReadLock`: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- `Pool<R>` with `Pool::Handle` RAII, `Factory`/`Validator` callbacks: [verified/04-libutil-misc.md](verified/04-libutil-misc.md).
- `ThreadPool` with `enqueue`/`process`/`shutdown`, the parallel `processGraph<T>` helper: [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md).
- The `nix::asio` namespace alias for `boost::asio`, `callbackToAwaitable` adapter, `forEachAsync`, `Callback<T>` (`callback.hh`): [verified/03-libutil-runtime.md](verified/03-libutil-runtime.md), [verified/04-libutil-misc.md](verified/04-libutil-misc.md).
- The bespoke `Goal::Co` coroutine machinery used in the build system: [verified/10-libstore-build.md](verified/10-libstore-build.md).

### The `Store` class hierarchy

- The abstract `Store` and `StoreConfig` base, the registry (`StoreFactory`/`Implementations::registered()`), `RegisterStoreImplementation<TConfig>`: [verified/05-libstore-core.md](verified/05-libstore-core.md).
- `LocalStore` (SQLite), `LocalFSStore` (filesystem accessor mix-in), `LocalOverlayStore` (lower/upper layered), `IndirectRootStore`, `GcStore`, `LogStore`, `DummyStore` (in-memory), `RestrictedStore` (recursive-nix sandbox view): [verified/07-libstore-local.md](verified/07-libstore-local.md).
- `RemoteStore` (worker-protocol pool), `UDSRemoteStore` (unix socket), `SSHStore`/`MountedSSHStore` (worker-proto over SSH), `LegacySSHStore` (serve-proto over SSH): [verified/08-libstore-remote.md](verified/08-libstore-remote.md).
- `BinaryCacheStore` (abstract), `LocalBinaryCacheStore`, `HttpBinaryCacheStore`, `S3BinaryCacheStore`: [verified/08-libstore-remote.md](verified/08-libstore-remote.md).

### Derivation parsing / serialisation / options

- Derivation ATerm parser/unparser (`parseDerivation`, `Derivation::unparse`, `readDerivation`, `writeDerivation`), `DerivationOutput` (variant of `InputAddressed`/`CAFixed`/`CAFloating`/`Deferred`/`Impure`), `DerivationType` (variant of `InputAddressed`/`ContentAddressed`/`Impure`), `DrvHashModulo`/`hashDerivationModulo`/`drvHashes` global, `Derivation::tryResolve`, JSON serialiser (`expectedJsonVersionDerivation = 4`): [verified/06-libstore-derivations.md](verified/06-libstore-derivations.md).
- `DerivationOptions<Input>` template (with `OutputChecks`, allowedReferences, sandbox profile, etc.) and the `tryResolve` for `DerivationOptions<SingleDerivedPath>` → `<StorePath>`: [verified/06-libstore-derivations.md](verified/06-libstore-derivations.md).
- `StructuredAttrs` and `prepareStructuredAttrs`: [verified/06-libstore-derivations.md](verified/06-libstore-derivations.md).

### Wire protocols (worker / serve / common)

All three live in [verified/09-libstore-protocol.md](verified/09-libstore-protocol.md):

- `WorkerProto` (37 live opcodes including `BuildPaths`, `BuildPathsWithResults`, `AddToStore`, `AddToStoreNar`, `AddMultipleToStore`, `RegisterDrvOutput`, `QueryRealisation`, `AddBuildLog`), `WorkerProto::Version` with `FeatureSet` partial ordering and `featureRealisationWithPath` / `featureDeleteDeadSpecificReferrers` / `featureDisableSetOptions`, the `BasicConnection`/`BasicClientConnection`/`BasicServerConnection`, the daemon `performOp` switch in [verified/09-libstore-protocol.md](verified/09-libstore-protocol.md).
- `ServeProto` (8 live commands; total ordering on `Version`; no `FeatureSet`).
- `CommonProto` (shared `string`/`StorePath`/`ContentAddress`/`DrvOutput`/`Realisation`/`Signature`/`BuildResultStatus` plus all `vector`/`set`/`tuple`/`map` containers); the `buildResultStatusTable` wire-tag table.

### Goal hierarchy + Worker

All in [verified/10-libstore-build.md](verified/10-libstore-build.md):

- `Goal` (abstract), `Goal::Co` coroutine wrapper, `JobCategory` (Build/Substitution/Administration), `ExitCode` (`ecBusy`/`ecSuccess`/`ecFailed`/`ecNoSubstituters`).
- Concrete goal types: `DerivationTrampolineGoal` (`"da$..."`), `DerivationGoal` (`"db$..."`), `DerivationResolutionGoal` (`"dc$..."`), `DerivationBuildingGoal` (`"dd$..."`), `PathSubstitutionGoal` (`"a$..."`), `DrvOutputSubstitutionGoal` (`"a$..."`).
- `Worker` (the coordinator) with per-goal-kind caches and `Worker::Waker` cross-thread wake mechanism (POSIX `unix::SelfPipe` vs Windows IO completion port).

### DerivationBuilder hierarchy

In [verified/10-libstore-build.md](verified/10-libstore-build.md):

- `DerivationBuilder` (abstract; inherits `RestrictionContext`), `DerivationBuilderImpl` (Unix base; the FIXME notes a planned rename to `UnixDerivationBuilder`), the platform diamond `ChrootDerivationBuilder` × {`LinuxDerivationBuilder` → `ChrootLinuxDerivationBuilder`, `FreeBSDDerivationBuilder` → `ChrootFreeBSDDerivationBuilder`}, plus `DarwinDerivationBuilder` (sibling), `ExternalDerivationBuilder` (sibling, gated on `Xp::ExternalBuilders`), `DerivationBuilderCallbacks` (log-file/child-termination delegation seam), `DerivationBuilderDeleter` (custom deleter that invokes `cleanupOnDestruction`).

### Value / EvalState / AttrSet

All in [verified/11-libexpr-eval.md](verified/11-libexpr-eval.md):

- `Value` discriminated-union with two storage layouts (generic 24-byte, x86_64 16-byte bit-packed via SSE2 atomic loads), the `NIX_VALUE_STORAGE_FOR_EACH_FIELD` X-macro driving everything.
- `EvalState`, `EvalMemory`, `Env`, the static (`StaticEvalSymbols::s`) vs dynamic symbol tables, the layered `Bindings` with `numLayers`/`baseLayer` chain (capped at `maxLayers = 8`).
- The `Counter` cache-line-aligned atomic counters gated by `NIX_SHOW_STATS`.

### Parser / Lexer / AST

In [verified/12-libexpr-parse.md](verified/12-libexpr-parse.md):

- `lexer.l` (flex), `lexer-helpers.{cc,hh}`, `parser.y` (bison), `parser-state.hh`/`parser-scanner-decls.hh`.
- The `Expr` AST hierarchy in `nixexpr.{cc,hh}`: `ExprInt`/`ExprFloat`/`ExprString`/`ExprPath`/`ExprVar`/`ExprSelect`/`ExprOpHasAttr`/`ExprAttrs`/`ExprList`/`ExprLambda`/`ExprCall`/`ExprLet`/`ExprWith`/`ExprIf`/`ExprAssert`/`ExprOpEq`/`ExprOpNEq`/`ExprOpAnd`/`ExprOpOr`/`ExprOpImpl`/`ExprOpUpdate`/`ExprOpConcatLists`/`ExprConcatStrings`/`ExprPos`, plus the `MakeBinOp` macro that synthesises six of the binop nodes.
- The four printers (`Printer::print`, `printAmbiguous`, `printValueAsJSON`, `printValueAsXML`) and their per-`nValueType` switches.

### Primops (full list)

In [verified/13-libexpr-primops.md](verified/13-libexpr-primops.md):

- `primops.cc` is the bulk: type predicates (`isNull`/`isBool`/`isInt`/`isFloat`/`isString`/`isPath`/`isAttrs`/`isList`/`isFunction`), `typeOf`, arithmetic (`add`/`sub`/`mul`/`div`/`lessThan`/`bitAnd`/`bitOr`/`bitXor`/`ceil`/`floor`), strings/regex (`stringLength`/`substring`/`replaceStrings`/`split`/`match`/`hashString`/`hashFile`/`toJSON`/`fromJSON`), attrset (`attrNames`/`attrValues`/`getAttr`/`hasAttr`/`isAttrs`/`removeAttrs`/`intersectAttrs`/`zipAttrsWith`/`mapAttrs`/`listToAttrs`/`catAttrs`), lists (`length`/`elemAt`/`head`/`tail`/`map`/`filter`/`foldl'`/`any`/`all`/`elem`/`concatLists`/`concatMap`/`partition`/`groupBy`/`sort`/`genList`), control flow (`abort`/`throw`/`tryEval`/`addErrorContext`/`break`/`scopedImport`/`import`/`derivationStrict`/`derivation`/`exec`), file (`readFile`/`readFileType`/`pathExists`/`baseNameOf`/`dirOf`/`readDir`/`outputOf`/`storePath`/`hashFile`/`filterSource`/`path`/`toFile`), env (`getEnv`/`getContext`/`appendContext`), version (`compareVersions`/`parseDrvName`/`splitVersion`), etc. About 130 names registered via `RegisterPrimOp`.
- Fetcher primops in dedicated files: `prim_fetchClosure` (`fetchClosure.cc`), `prim_fetchMercurial` (`fetchMercurial.cc`), `fetchTree`/`fetch`/`prim_forceLazyFetcherAttr` (`fetchTree.cc`), `prim_fromTOML` (`fromTOML.cc`), the string-context primops (`getContext`/`appendContext`/`unsafeDiscardOutputDependency`/`addDrvOutputDependencies`) in `context.cc`.

### InputScheme / fetchers

In [verified/14-libfetchers.md](verified/14-libfetchers.md):

- The `InputScheme` abstract base and its eight concrete subclasses (`IndirectInputScheme`, `PathInputScheme`, `GitInputScheme`, `MercurialInputScheme`, `GitArchiveInputScheme` → `GitHubInputScheme`/`GitLabInputScheme`/`SourceHutInputScheme`, `CurlInputScheme` → `FileInputScheme`/`TarballInputScheme`).
- The `Cache` (SQLite at `fetcher-cache-v4.sqlite`) and `InputCache` (in-memory) layers.
- `git-utils.cc` wrapping libgit2: `GitRepoImpl`, `GitSourceAccessor`, `GitFileSystemObjectSinkImpl`, the workdir-info cache.
- LFS fetch (`git-lfs-fetch.cc`).
- Filtering and allow-list source accessors.
- `fetch-to-store` (the SQLite-backed source-path-to-store-path memoisation).
- The registry (`registry.cc`) and `getCustomRegistry`.

### Flake machinery

In [verified/15-libflake-libmain.md](verified/15-libflake-libmain.md):

- `Flake`, `LockedFlake`, `LockFile`, `Node` (lock-file node), `Input` (flake input).
- `FlakeRef`, `parseFlakeRef`, `parseFlakeRefWithFragment`, `fromAttrs`/`toAttrs`, `applyOverrides`.
- `flake::Settings`, `acceptFlakeConfig`, `commitLockFileSummary`, the `whitelist` of trusted `nixConfig` keys.
- `flake-primops.cc`: `prim_flakeRefToString`, `prim_parseFlakeRef`, `prim_callFlake`, `prim_getFlake`.
- `url-name.cc`: `getNameFromURL`.

### Command base classes / Installable hierarchy

All in [verified/16-libcmd.md](verified/16-libcmd.md):

- `Command` (abstract; inherited from libutil), `NixMultiCommand`, `StoreConfigCommand`, `StoreCommand`, `EvalCommand`, `CopyCommand`, `MixFlakeOptions`, `SourceExprCommand`, `RawInstallablesCommand`, `InstallablesCommand`, `BuiltPathsCommand`, `StorePathsCommand`, `StorePathCommand`, `InstallableCommand`, `InstallableValueCommand`.
- The mixins: `MixEvalArgs`, `MixRepair`, `MixReadOnlyOption`, `MixOperateOnOptions`, `MixProfile`, `MixDefaultProfile`, `MixEnvironment`, `MixNoCheckSigs`, `MixOutLinkBase`, `MixOutLinkByDefault`.
- `Installable` (abstract), `InstallableValue` (intermediate), `InstallableAttrPath`, `InstallableFlake`, `InstallableDerivedPath`.
- `BuiltPath`/`SingleBuiltPath` and their `Built`/`Opaque` variants.
- `RegisterCommand` (modern `nix` registry) and `RegisterLegacyCommand` (legacy `nix-*` registry).

### Modern `nix` subcommands

Split across [verified/17-nix-modern-1.md](verified/17-nix-modern-1.md) and
[verified/18-nix-modern-2-legacy.md](verified/18-nix-modern-2-legacy.md):

- Top-level evaluation/build commands (shard 17): `nix build`, `nix eval`, `nix repl`, `nix search`, `nix develop`/`nix shell`/`nix print-dev-env`, `nix run`, `nix bundle`, `nix env shell`, `nix flake init/check/show/lock/metadata/clone/archive/update/prefetch-inputs`, `nix registry add/list/pin/remove`, `nix profile add/list/remove/upgrade/rollback/wipe-history/diff-closures`, `nix path-info`, `nix path-from-hash-part`, `nix store-prefetch-file`, `nix realisation info`, `nix derivation show/add`, `nix edit`, `nix config show/check`, `nix diff-closures`, `nix why-depends`, `nix log`, `nix fmt`/`nix formatter`, `nix upgrade-nix`, `nix hash` (path/file/convert).
- `nix store ...` subtree and the NAR/key tooling (shard 18): `nix store info/gc/delete/repair/copy-log/optimise/verify/cat/ls/dump-path/add/add-file/add-path/make-content-addressed/sign`, `nix copy`, `nix nar pack/cat/ls`, `nix key generate-secret/convert-secret-to-public`, `nix daemon`, `nix store-roots-daemon`.

### Legacy `nix-*` tools

In [verified/18-nix-modern-2-legacy.md](verified/18-nix-modern-2-legacy.md):

- `nix-build`, `nix-instantiate`, `nix-env` (with its own `Operation` dispatch table; 12 entries), `nix-store` (25-entry op table), `nix-channel`, `nix-collect-garbage`, `nix-copy-closure`, `build-remote`, plus the helpers `nix-store/dotgraph.{cc,hh}` and `nix-store/graphml.{cc,hh}`.

### C ABI surface

In [verified/19-c-bindings-misc.md](verified/19-c-bindings-misc.md):

- One init function per library (`nix_libutil_init`, `nix_libstore_init`, `nix_libstore_init_no_load_config`, `nix_libexpr_init`, `nix_init_plugins`).
- Opaque pointer wrappers: `Store`, `StorePath`, `nix_derivation`, `BindingsBuilder`, `ListBuilder`, `nix_string_return`, `nix_printer`, `nix_string_context`, `nix_fetchers_settings`, `nix_flake_settings`, `nix_flake_reference`, `nix_flake_lock_flags`, `nix_locked_flake`, `nix_flake_reference_parse_flags`, `nix_realised_string`, `nix_c_context`, plus `EvalState` and `nix_value` (the two outliers with extra fields).
- The `NIXC_CATCH_ERRS{,_RES,_NULL}` error-protocol macros and the `nix_set_err_msg`/`nix_clear_err` helpers.
- The "forced" vs `_lazy` access pattern in `nix_api_value` (`nix_get_list_byidx{,_lazy}`, `nix_get_attr_byname{,_lazy}`, `nix_get_attr_byidx{,_lazy}`).
- Refcounting of GC-managed handles (`nix_gc_incref`/`nix_gc_decref`) backed by a `boost::concurrent_flat_map nix_refcounts`.

## Consolidated duplication and refactoring candidates

Numbered, prioritised list of refactoring opportunities surfaced by the
per-shard verified docs. Pointers identify the shards where the relevant
code lives.

### Duplicated wire / serialisation logic

1. **`BuildResult` serialiser duplicated across worker and serve protocols.** Both `WorkerProto::Serialise<BuildResult>` and `ServeProto::Serialise<BuildResult>` are near-byte-identical (same status / errorMsg / timing / builtOutputs sequence, only the version-cutoff literals differ — worker uses `{1,29}`/`{1,37}`/feature `realisation-with-path-not-hash`/`{1,28}`; serve uses `{2,3}`/`{2,8}`/`{2,6}`). Worker has an extra cpu-timing branch and a feature gate; serve does not. The shared `common = [&](errorMsg, isNonDeterministic, builtOutputs) { ... }` lambda is duplicated almost verbatim. The single biggest refactor target in the protocol layer.
   - verified/09-libstore-protocol.md
2. **`UnkeyedValidPathInfo` serialiser duplicated across worker and serve protocols.** Both protocols hand-roll the deriver/refs/narHash/narSize/sigs/ca format with cosmetic differences: worker uses `Serialise<std::optional<StorePath>>` for the deriver; serve uses an empty-string sentinel inline; worker uses Base16 narHash without prefix; serve uses Nix32 narHash with prefix; serve emits narSize twice (the second as the obsolete "downloadSize"); worker emits an `ultimate` flag, serve does not.
   - verified/09-libstore-protocol.md
3. **`DrvOutput`/`UnkeyedRealisation`/`Realisation` serialisers identical between worker and serve.** Each pair is byte-identical except for the version-gate (`featureRealisationWithPath` for worker, `>= 2.8` for serve). Obvious candidate for promotion to `CommonProto` once a way to thread the per-protocol gate through is found.
   - verified/09-libstore-protocol.md
4. **Length-prefixed container serialiser macros are three near-identical copies.** `WORKER_USE_LENGTH_PREFIX_SERIALISER`, `SERVE_USE_LENGTH_PREFIX_SERIALISER`, and `COMMON_USE_LENGTH_PREFIX_SERIALISER` each emit `Serialise<vector<T>>`/`Serialise<set<T>>`/`Serialise<tuple<Ts...>>`/`Serialise<map<K,V>>` specialisations delegating to `LengthPrefixedProtoHelper<Proto, T>`. The macros could be a single template parametrised on the protocol struct.
   - verified/09-libstore-protocol.md
5. **`DECLARE_*_SERIALISER` declaration macros are three near-identical copies.** `DECLARE_COMMON_SERIALISER`, `DECLARE_WORKER_SERIALISER`, `DECLARE_SERVE_SERIALISER` differ only in the namespace prefix on `Serialise<T>` and (cosmetically) in the parameter name. Same shape as #4.
   - verified/09-libstore-protocol.md
6. **`GET_PROTOCOL_MAJOR`/`GET_PROTOCOL_MINOR` macros duplicated.** Defined identically in both `worker-protocol.hh` and `serve-protocol.hh` (`(x) & 0xff00` and `(x) & 0x00ff`). Including both headers in the same TU works only because the second `#define` produces an identical token sequence.
   - verified/09-libstore-protocol.md
7. **Protocol handshake logic is parallel between worker and serve.** `WorkerProto::BasicClientConnection::handshake` and `ServeProto::BasicClientConnection::handshake` both send magic-1, read magic-2, exchange version numbers, take the min. Worker additionally exchanges and intersects a `FeatureSet` (≥1.38) via private `intersectFeatures`; serve has no such step. Server-side mirrors are likewise parallel.
   - verified/09-libstore-protocol.md
8. **Three encoders for the same `<algo>:<base>` shape.** `Hash::to_string`/`Hash::parseAny*` use `<algo>:<base*>` (and SRI `<algo>-<base64>`); `Signature` and `Key` use `<name>:<base64>` via the anon-namespace `parseColonBase64`/`serializeColonBase64`. Each module rolls its own. A shared "colon-prefixed Base-N" helper would consolidate these.
   - verified/02-libutil-data.md
9. **JSON adl_serializer scaffolding is split across multiple files.** `json-impls.hh` provides the macros, `json-non-null.hh` provides the `json_avoids_null<T>` trait, `json-utils.hh` provides accessor helpers and the generic `adl_serializer<std::optional<T>>`, `abstract-setting-to-json.hh` provides `BaseSetting<T>::toJSONObject`. Each consumer (`hash.cc`, `compression-settings.cc`, `signature/local-keys.cc`, plus most files in shard 05) writes near-identical adl_serializer boilerplate.
   - verified/02-libutil-data.md, verified/05-libstore-core.md
10. **`dumpPath`/`restorePath` overload sets scattered across compilation units.** `archive.hh/.cc` exposes `dumpPath(path, Sink, PathFilter)` plus `dumpPathAndGetMtime`; `source-path.hh/.cc` adds `SourcePath::dumpPath`; `source-accessor.hh/.cc` adds `SourceAccessor::dumpPath` (the actual NAR algorithm); `file-content-address.hh/.cc` adds method-dispatched `dumpPath(SourcePath, Sink, FileSerialisationMethod, PathFilter)`. Same shape for `restorePath`. No single header documents the relationship.
   - verified/01-libutil-io.md

### Parallel store implementations

11. **`LocalOverlayStore` repeats "try upper, fall through to lower" pattern in seven methods.** `queryPathInfoUncached`, `queryRealisationUncached`, `isValidPathUncached`, `queryPathFromHashPart`, `queryReferrers`, `queryValidDerivers`, plus `registerDrvOutput`/`registerValidPaths` (with copy-up). The two callback variants build a chained continuation through `Callback`/captured `callbackPtr`; the synchronous variants short-circuit on the upper hit. A helper for the synchronous shape would consolidate four functions.
    - verified/07-libstore-local.md
12. **Three "store wrapper" implementations with no shared abstract base.** `LocalOverlayStore` (delegating to `lowerStore`), `RestrictedStore` (delegating to `next`), `DummyStoreImpl` (no delegation; in-memory). Each redeclares large portions of the `Store` virtual surface. A "DelegatingStore" CRTP or non-virtual base would deduplicate.
    - verified/07-libstore-local.md
13. **Binary-cache subclasses duplicate the upsertFile/fileExists/getFile triple.** `LocalBinaryCacheStore`, `HttpBinaryCacheStore`, `S3BinaryCacheStore` each provide their own implementation, but the surrounding logic is mostly cloned: build paths from URL, wrap errors as a per-store `MakeError(UploadTo*)`, handle `NotFound`/`Forbidden` specifically. `HttpBinaryCacheStore::upsertFile` and `S3BinaryCacheStore::upsertFile` share an identical "if compress then compress + Content-Encoding else dispatch upload" skeleton.
    - verified/08-libstore-remote.md
14. **`RemoteFSAccessor` constructed identically by `RemoteStore::getRemoteFSAccessor` and `BinaryCacheStore::getRemoteFSAccessor`.** Both pass the `requireValidPath` flag; only difference is `BinaryCacheStore` plumbs through `config.localNarCache`. The corresponding `getFSAccessor` overrides are mechanical wrappers in both. ~10 lines could move into `Store`.
    - verified/08-libstore-remote.md
15. **`UDSRemoteStore` and `MountedSSHStore` mix `RemoteStore` with `LocalFSStore` identically.** Both override `getFSAccessor`/`narFromPath` with the same delegate-to-`LocalFSStore` calls. The only meaningful divergence is the GC-root strategy: `UDSRemoteStore` sends `WorkerProto::Op::AddIndirectRoot`; `MountedSSHStore` sends `WorkerProto::Op::AddPermRoot`.
    - verified/08-libstore-remote.md
16. **Three SSH-store classes share `CommonSSHStoreConfig` but each rolls its own `Connection`.** `LegacySSHStore::Connection`, `SSHStore::Connection`, and the worker-proto `Connection` inside `RemoteStore` each carry a `unique_ptr<SSHMaster::Connection> sshConn` and pipe wiring. `LegacySSHStore` reimplements its own `Pool<Connection>`, `connect`, `flushBadConnections`-equivalent (`good` flag), and stat reporting (`getConnectionStats`/`getConnectionPid`) instead of leveraging `RemoteStore`.
    - verified/08-libstore-remote.md

### Repeated boilerplate

17. **`anchor()` vtable-pinning override appears in twelve+ classes.** Every class with virtual functions in the libstore shards defines a private out-of-line `void anchor() override {}` whose only purpose is to pin the vtable. Used in `StoreConfig`, `Store`, `LocalStoreConfig`, `LocalBuildStoreConfig`, `LocalStore`, `LocalFSStoreConfig`, `LocalFSStore`, `LocalOverlayStoreConfig`, `LocalOverlayStore`, `IndirectRootStore`, `GcStore`, `LogStore`, `DummyStoreConfig`, `DummyStore`, `DummyStoreImpl`, `RestrictedStore`, `RemoteStoreConfig`, `RemoteStore`, `UDSRemoteStoreConfig`, `UDSRemoteStore`, `BinaryCacheStoreConfig`, `BinaryCacheStore`, `LocalBinaryCacheStoreConfig`, `LocalBinaryCacheStore`, `HttpBinaryCacheStoreConfig`, `HttpBinaryCacheStore`, `SSHStoreConfig`, `SSHStore`, `MountedSSHStoreConfig`, `MountedSSHStore`, `LegacySSHStoreConfig`, `LegacySSHStore`, `CommonSSHStoreConfig`, `S3BinaryCacheStore`. A macro or CRTP could eliminate this entirely.
    - verified/05-libstore-core.md, verified/07-libstore-local.md, verified/08-libstore-remote.md
18. **`MakeError(name, parent)` macro coexists with hand-written `CloneableError<Derived,Parent>` derivations.** Most error types use the macro; a handful (`ExecError`, `MissingExperimentalFeature`, `BuildError`, `BuilderFailureError`, `MissingRealisation`, `AwsAuthError`, `FileTransferError`, `InvalidSSHAuthority`, `NotDeterministic`, `BuildEnvFileConflictError`, `TimedOut`, `curlMultiError`, `SymlinkNotAllowed`, `SQLiteError`) bypass the macro to add fields. A CRTP base could subsume both styles. The `SystemError`/`SysError`/`WinError` triad's `DisambigHintFmt`/`DisambigVarArgs` tag idiom is similarly replicated.
    - verified/03-libutil-runtime.md, verified/04-libutil-misc.md
19. **`Setting<T>{this, default, name, R"(doc)", aliases, xpFeature}` initialisers dominate `*Config` boilerplate.** Every store config and settings struct uses this same shape; explicit instantiations for each `T` are scattered across many files. A small flag-builder DSL or constexpr table would shrink the source dramatically, especially in `LocalSettings`, `Settings`, `RemoteStoreConfig`, `BinaryCacheStoreConfig`, `HttpBinaryCacheStoreConfig`, `S3BinaryCacheStoreConfig`, `WorkerSettings`, `flake::Settings`, the `Mix*` command tower.
    - verified/03-libutil-runtime.md, verified/07-libstore-local.md, verified/08-libstore-remote.md, verified/09-libstore-protocol.md, verified/16-libcmd.md
20. **`MAKE_WRAPPER_CONSTRUCTOR(T)` + `Raw raw` member + visit-with-overloaded idiom appears for every variant-shaped type.** `ContentAddressMethod`, `ContentAddressWithReferences`, `OutputsSpec`, `ExtendedOutputsSpec`, `SingleDerivedPath`/`DerivedPath`, `RealisedPath`, `StorePathWithOutputs::ParseResult`, `StoreReference::Variant`, `BuildResult::inner`, `DrvRef<Item>`, `DerivationOutput::Raw`, `DerivationType::Raw`, `DrvHashModulo::Raw`. Each pair re-defines `==`, `to_string`, and `parse` symmetrically. A tagged-union helper would consolidate.
    - verified/05-libstore-core.md, verified/06-libstore-derivations.md
21. **`Setting<T>` BaseSetting specialisations have a four-times-cut-and-pasted shape.** Each specialisation declares `BaseSetting<T>::trait`, defines `parse`/`to_string`, and (for collections) `appendOrSet`. Repeated for `SandboxMode`, `PathsInChroot`, `LocalSettings::ExternalBuilders`, `StoreReference`, `std::vector<StoreReference>`, `std::set<StoreReference>`, `CompressionAlgo`, `std::optional<CompressionAlgo>`, `S3AddressingStyle`, `Diagnose`. There is no shared template for "tokenise-list setting" or "JSON-roundtripping setting".
    - verified/02-libutil-data.md, verified/07-libstore-local.md, verified/08-libstore-remote.md, verified/11-libexpr-eval.md
22. **Async/sync pair pattern.** `Store::queryPathInfo` and `Store::queryRealisation` each have a synchronous `promise/future` wrapper around a `Callback`-based async variant; the wrapper code is structurally identical and could be factored into a helper template that turns any async callback into a blocking call.
    - verified/05-libstore-core.md
23. **`unsupported(...)` is used as a default for many virtuals.** `Store::queryAllValidPaths`, `Store::queryReferrers`, `Store::addSignatures`, plus the `*::repairPath` and most overrides in `RestrictedStore` and `LegacySSHStore`. This is a workaround in lieu of pure-virtual + capability traits; explicit feature negotiation would be cleaner.
    - verified/05-libstore-core.md
24. **`registerCommand<...>` boilerplate is uniform across 30+ command files.** Every modern `nix` command file ends with `static auto rXxx = registerCommand<CmdXxx>("xxx")`, with naming irregular (`rCmdXxx`, `r2`, `rFormatterRun`, `rShowConfig`, etc.). The legacy bridges follow the same template via `RegisterLegacyCommand`. A macro encoding name + class + category could shrink each file by several lines.
    - verified/17-nix-modern-1.md, verified/18-nix-modern-2-legacy.md
25. **`doc()` overrides include a per-command `*.md` (or `*.md.gen.hh`) via `#include`.** Every command does `return ` `#include "name.md"` `;`. Stable but heavy boilerplate.
    - verified/17-nix-modern-1.md, verified/18-nix-modern-2-legacy.md

### Per-platform symmetry

26. **`pathlocks` Unix vs Windows.** `lockPaths` (sorted iteration, retry on stale lock detected by `<file>.lock` not being empty) and `FdLock`'s constructor (try non-blocking, then blocking with `printInfo(waitMsg)`) are duplicated almost verbatim between `unix/pathlocks.cc` and `windows/pathlocks.cc`. Differences are confined to `flock` vs `LockFileEx` and the test for "lock file became stale" (`fstat().st_size` vs `getFileSize`). Could share a generic skeleton with platform shims for `openLockFile`/`lockFile`/`isStale`.
    - verified/07-libstore-local.md
27. **File-system / file-descriptor / processes per-platform implementations parallel each other.** `unix/file-system.cc` vs `windows/file-system.cc`; `unix/file-descriptor.cc` vs `windows/file-descriptor.cc`; `unix/processes.cc` vs `windows/processes.cc`; `unix/environment-variables.cc` vs `windows/environment-variables.cc`; `unix/users.cc` vs `windows/users.cc`. Each pair declares the same logical API but with different signatures (Windows takes wide strings as `OsString`; `Pipe::create` has no `nonBlocking` parameter on Windows; `MuxablePipePollState::poll` takes an extra `HANDLE ioport` on Windows; `Pid` holds a `pid_t` on Unix and an `AutoCloseFD` on Windows).
    - verified/04-libutil-misc.md
28. **Goal builders by platform form a diamond hierarchy.** `DerivationBuilderImpl` is the unix base; `ChrootDerivationBuilder` is the Linux/FreeBSD shared chroot fragment; `LinuxDerivationBuilder`/`FreeBSDDerivationBuilder` are the platform-specific siblings; `ChrootLinuxDerivationBuilder`/`ChrootFreeBSDDerivationBuilder` are the diamond merges; `DarwinDerivationBuilder` and `ExternalDerivationBuilder` sit beside as siblings. Many overrides (`setBuildTmpDir`, `tmpDirInSandbox`, `prepareSandbox`, `enterChroot`, `setUser`, `startChild`, `cleanupBuild`, `killSandbox`, `getBuildUser`) follow a pattern of "base then platform-specific suffix" that could be expressed as separable strategy hooks.
    - verified/10-libstore-build.md
29. **XDG dirs (Unix) vs known folders (Windows).** `nix::unix::xdg::getCacheHome`/`getConfigHome`/`getConfigDirs`/`getDataHome`/`getStateHome` parallel `nix::windows::known_folders::getLocalAppData`/`getRoamingAppData`/`getProgramData`. The wrapping `getCacheDir`/`getConfigDir`/... in `users.cc` switches on `_WIN32` per call. A platform-conditional table would be cleaner.
    - verified/03-libutil-runtime.md, verified/04-libutil-misc.md

### Inheritance chains worth flattening

30. **`Installable` chain (Installable → InstallableValue → InstallableAttrPath / InstallableFlake; InstallableDerivedPath sibling).** The two leaves' `toDerivedPaths` perform the same `ExtendedOutputsSpec` visit (`Default` synthesises `outputsToInstall` then defaults to `{"out"}`; `Explicit` returns the spec verbatim) and the same `trySinglePathToDerivedPaths` short-circuit. Extracting an `outputsSpecFromExtended` helper plus a templated `InstallableValue` method would deduplicate.
    - verified/16-libcmd.md
31. **Command `run` chain (StoreConfigCommand → StoreCommand → BuiltPathsCommand → StorePathsCommand → StorePathCommand).** Each step does nothing more than narrow the type via virtual `run` overloads. The chain is fragile to GCC's `-Woverloaded-virtual` warning (suppressed by a pragma in `command.hh`). A templated CRTP-style narrowing would flatten it.
    - verified/16-libcmd.md
32. **`InputScheme` hierarchy with eight schemes and significant per-scheme rewrite.** Every scheme registers itself at startup via `static auto rXxx = OnStartup([] { registerInputScheme(make_unique<XxxInputScheme>()); });` (the same idiom verbatim across all eight). Every scheme reimplements URL parsing, attribute strip+re-emit, cache-key construction, fingerprint computation. A `BaseInputScheme<T>` CRTP with `urlGrammar`, `cacheKeyDomain`, etc. would replace much of the duplication.
    - verified/14-libfetchers.md
33. **`Goal` hierarchy (six concrete goal types).** Common shape across all six: `init`/`gaveUpOnSubstitution`/`tryToBuild`/etc. coroutines, `key` override, `jobCategory` override, `doneSuccess`/`doneFailure` book-keeping calling `worker.doneBuilds++` etc., `MaintainCount<uint64_t>` counter handles. `DerivationGoal::doneSuccess` and `DerivationBuildingGoal::doneSuccess` share their body verbatim with only the counter handle name differing.
    - verified/10-libstore-build.md
34. **`DerivationBuilder` diamond (see #28).** A more disciplined strategy/policy split (cgroup strategy vs jail strategy vs sandbox-init strategy, plus a separate "needs hash rewrite" trait) would let a single `UnixDerivationBuilder` be parameterised on the platform pieces.
    - verified/10-libstore-build.md
35. **`Store` hierarchy with seven concrete stores (LocalStore, LocalOverlayStore, DummyStore, RestrictedStore, RemoteStore, UDSRemoteStore, BinaryCacheStore + 3 leaves, SSHStore + 1 leaf, LegacySSHStore).** Many concrete stores are "thin override + delegate" or "diamond merge" classes. There is no shared "DelegatingStore" or "FSAccessorStore" base. See #11, #12, #14, #15.
    - verified/05-libstore-core.md, verified/07-libstore-local.md, verified/08-libstore-remote.md

### Multi-implementation patterns that could share a base

36. **Four parallel value renderers.** `Printer::print` (`print.cc`), `printAmbiguous` (`print-ambiguous.cc`), `printValueAsJSON` (`value-to-json.cc`), `printValueAsXML` (`value-to-xml.cc`), plus the AST-side `Expr::show` (`nixexpr.cc`). All five share: `state.forceValue` (or check `force`/`strict` option), `state.settings.maxCallDepth` / `addCallDepth(...)` recursion guard, recursion with a per-printer `seen`/`drvsSeen`/`Done` set, derivation special-casing (`state.s.drvPath` / `state.isDerivation`), per-`nValueType` switch. A shared visitor scaffold could leave each printer implementing only per-type emission.
    - verified/12-libexpr-parse.md
37. **NAR-tree walkers in three places.** `NarAccessorImpl::find/get` (in `nar-accessor.cc`), `NarIndexer::createMember` (in `nar-listing.cc`), `MemorySourceAccessor::open` (in `memory-source-accessor.cc`). All walk a path, descend into the `Directory` variant, and either find or insert. A generic helper over `fso::VariantT` could replace all three.
    - verified/01-libutil-io.md
38. **Source/Sink wrapper hierarchy with many parallel one-shot adapters.** `TeeSink`/`TeeSource`, `LengthSink`/`LengthSource`, `LambdaSink`/`LambdaSource`, `SizedSource`, `EnsureRead`, `ChainSource`. Written one-by-one; a base or generator could reduce repetition.
    - verified/01-libutil-io.md
39. **`MemorySink`/`RestoreSink` parallel `FileSystemObjectSink` impls.** Both implement essentially the same `FileSystemObjectSink` interface plus their own `CreateRegularFileSink` subclass for byte streams (`RestoreRegularFile` and `CreateMemoryRegularFile`). The bytes-callback boilerplate could be factored.
    - verified/01-libutil-io.md
40. **Wrapping source accessors clear `displayPrefix` and chain `showPath`/`getPhysicalPath`/`invalidateCache` near-identically.** `UnionSourceAccessor`, `MountedSourceAccessorImpl`, `CachingSourceAccessor` all redo this in their constructors. Their `getFingerprint` implementations diverge: caching is a passthrough; mounted/union apply "own fingerprint else delegate". A `WrappingSourceAccessor` base could absorb all three.
    - verified/01-libutil-io.md
41. **Logger fan-out duplication.** `SimpleLogger`, `JSONLogger`, `TeeLogger` independently implement `startActivity`/`stopActivity`/`result`/`log`/`logEI`/`writeToStdout`/`ask`/`setPrintBuildLogs`. A common base or visitor would deduplicate.
    - verified/03-libutil-runtime.md
42. **`Config`/`GlobalConfig` plumbing duplication.** `Config::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` and `GlobalConfig::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` follow identical loop shapes (one subtle behaviour difference in `toKeyValue`: `Config` only emits aliases; `GlobalConfig` calls `globalConfig.getSettings(...)` and emits all entries). A CRTP base or composition would let `GlobalConfig` simply iterate registered configs.
    - verified/03-libutil-runtime.md
43. **X-macro setting list.** The `BaseSetting<T>` template-specialisation pattern (`parse`, `to_string`, `appendOrSet`, `convertToArg`, `trait::appendable`, `NIX_DECLARE_CONFIG_SERIALISER`, explicit `template class BaseSetting<...>` instantiations) is replicated for many `T` in `configuration.cc`. The list (`std::list<path>`, `Strings`, `StringSet`, `std::set<path>`, `std::set<ExperimentalFeature>`, `StringMap`, `AbsolutePath`, plus the per-store specialisations) plus the trait specialisations plus the macro plus the explicit instantiations form four near-parallel registers. A single X-macro list would collapse them.
    - verified/03-libutil-runtime.md, verified/07-libstore-local.md
44. **`BuildLog` and `LogSink` are similar line-buffering sinks.** `BuildLog` (in `build-log.{cc,hh}`) and `LogSink` (in `derivation-building-goal.cc`) both implement `Sink::operator()` as a line buffer that splits on `\n`. `BuildLog` adds JSON parsing, tail tracking, and `\r` carriage-return handling; `LogSink` is simpler. They could share a base.
    - verified/10-libstore-build.md
45. **Three NAR magic-prefixed switches.** `archive.cc` and `nar-listing.cc` independently implement near-identical switches over file-system-object types (regular/directory/symlink/...); `tarfile.cc` does the same over `archive_entry_filetype`. A unifying visitor over the tri-typed FSO tag would reduce repetition.
    - verified/01-libutil-io.md
46. **`builtinBuilders` family share a `RegisterBuiltinBuilder` registration pattern.** All three (`buildenv`, `fetchurl`, `unpack-channel`) follow the same shape: static function `void(const BuiltinBuilderContext &)` plus a file-scope `static RegisterBuiltinBuilder` instance. Each uses a local `getAttr` lambda for env-attribute lookup. The local lambda is a candidate for a shared helper.
    - verified/10-libstore-build.md
47. **`*Goal::doneSuccess`/`doneFailure` patterns.** Every `*Goal` defines a `doneSuccess`/`doneFailure` pair that drops `MaintainCount` handles, increments `worker.doneBuilds++` / `failedBuilds++`, calls `exitStatusFlags.updateFromStatus`, calls `updateProgress`, then forwards to `Goal::doneSuccess`/`doneFailure`. `DerivationBuildingGoal` and `DerivationGoal` share this pattern verbatim.
    - verified/10-libstore-build.md

### Dead or stale code

48. **`void check();` in `lockfile.cc` is an unused forward declaration.** Inside `nix::flake` after `LockFile::check`'s definition; appears to be dead code.
    - verified/15-libflake-libmain.md
49. **`blockInt` and similar dead identifiers.** Worth a sweep through the verified shards for any declaration without matching uses.
50. **`nix_value_incref`/`_decref` are pure forwarders to the generic `nix_gc_*` helpers.** Documented as the preferred typed API, but currently identical to the generic ones. The header comment notes a migration intent.
    - verified/19-c-bindings-misc.md
51. **`#if 0` blocks in `github.cc`.** The treeHash-mismatch warning inside `downloadArchive` and the treeHash output attribute inside `getAccessor` are commented out, hinting at unfinished tree-hash propagation.
    - verified/14-libfetchers.md
52. **`CurlInputScheme::specialParams` is declared but never defined or referenced.** Compiles only because nothing odr-uses it.
    - verified/14-libfetchers.md
53. **`getCustomRegistry` only honours the first call's path.** Memoises in a function-local static; later changes to the registry path are silently ignored.
    - verified/14-libfetchers.md
54. **`SQLiteSettings::useWAL` is declared without a default initialiser.** Each constructor of `SQLite` reads it; a constructor that doesn't set it is undefined behaviour.
    - verified/07-libstore-local.md
55. **Stale `IndexReferrer` index dropped at runtime.** The `20260309-drop-redundant-indexreferrer` migration in `LocalStore::upgradeDBSchema` cleans up a previous-version index. The matching `create index` is no longer in `schema.sql`.
    - verified/07-libstore-local.md
56. **`BaseSetting<PathsInChroot>::trait` lives in a different file from `BaseSetting<SandboxMode>::trait`.** The `PathsInChroot` trait is in `local-settings.hh`; `SandboxMode` is only in `globals.cc`. Inconsistent placement; one of them should move.
    - verified/07-libstore-local.md
57. **Macro hygiene caveats.** All three `*_USE_LENGTH_PREFIX_SERIALISER_COMMA` helpers (`WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA`, `SERVE_USE_LENGTH_PREFIX_SERIALISER_COMMA`) are `#define`d but never `#undef`'d in their respective impl headers, leaking into translation units. There is also a stray bare `#undef COMMA_` at the end of `common-protocol.hh` with no matching `#define` in scope.
    - verified/09-libstore-protocol.md
58. **`getMaxCPU` catches `Error` and routes through `ignoreExceptionInDestructor`, but it is not actually a destructor.** Should use `ignoreExceptionExceptInterrupt` per the `util.hh` comment.
    - verified/03-libutil-runtime.md
59. **`useBuildUsers` returns a function-local `static bool`.** Computed once and cached for the process lifetime; changes to `localSettings` after first call are not observed.
    - verified/07-libstore-local.md
60. **`SQLiteStmt::create` is called on `purgeCache` but the statement is never used.** In `NarInfoDiskCacheImpl::State`; the periodic purge runs ad-hoc inside the constructor against `LastPurge` rather than via the prepared statement.
    - verified/08-libstore-remote.md

### Duplicated parsers / regexes

61. **Two regex stash points for git-style refs/revisions.** `url-parts.hh` declares `refRegexS`/`revRegexS`/`refAndOrRevRegex` with `extern std::regex refRegex`/`revRegex` defined in `url.cc`; `git.cc::parseLsRemoteLine` defines its own anonymous `std::regex line_regex`. The `line_regex` matches a different shape (whole ls-remote line) so a direct merge isn't possible, but a shared tokeniser would help.
    - verified/02-libutil-data.md
62. **URL vs flakeref vs url-name parsers all walk similar URL shapes.** `tryParseScpStyle` (URL), `parseFlakeRef`/`fromParsedURL`/`parsePathFlakeRefWithFragment`/`parseFlakeIdRef` (flakeref), `getNameFromURL` (url-name), per-`InputScheme` URL handling (fetchers). Five places probe `github|gitlab|sourcehut`-style schemes and the `<owner>/<repo>` path layout.
    - verified/02-libutil-data.md, verified/14-libfetchers.md, verified/15-libflake-libmain.md
63. **Path resolution rules sit in two parallel places in libexpr.** `path_start` in `parser.y` (handles absolute, relative, and `~/` paths with their lint diagnoses) and `EvalState::rootPath`/`storePath` in `paths.cc`. The relative-path computation `CanonPath(literal, basePath.path).abs()` has the same shape as `EvalState::rootPath(string_view)`.
    - verified/12-libexpr-parse.md
64. **`Formals` and `FormalsBuilder` independently implement `has(Symbol)`.** Two parallel containers exist for function formals: `FormalsBuilder` (`std::vector<Formal>` + ellipsis, used during parsing) and `Formals` (`std::span<Formal>` + ellipsis, used post-allocation). Both implement `has(Symbol)` independently with the same lower-bound predicate.
    - verified/12-libexpr-parse.md
65. **`primop_*` argument-validation boilerplate is heavily duplicated.** Almost every `prim_*` opens with `state.forceValue`/`forceAttrs`/`forceList`/`forceString`/`forceStringNoCtx`/`forceBool`/`forceInt`/`forceFloat`, each with a hand-written "while evaluating the Nth argument passed to builtins.<name>" message. Same shape ~hundreds of times. A `validateArg(state, n, primop_name, type)` helper would shrink the binary.
    - verified/13-libexpr-primops.md
66. **`prim_isNull` … `prim_isPath` (8 type-predicate primops, plus `prim_isAttrs`/`prim_isList`/`prim_isFunction`).** All identical except for the enum-tag they compare against. A registration macro or table-driven approach would remove the boilerplate.
    - verified/13-libexpr-primops.md
67. **Numeric primops (`__add`/`__sub`/`__mul`/`__div`).** Each one is the same template instantiated four times: forceValue both args, dispatch on `nFloat` else int, check overflow with `valueChecked()`, raise `EvalError` with a slightly different verb.
    - verified/13-libexpr-primops.md
68. **`prim_ceil` and `prim_floor` are byte-for-byte twins** (only `ceil(value)` vs `floor(value)` differs). The precision-loss/overflow blocks and the GitHub issue link are identical.
    - verified/13-libexpr-primops.md

### Cache-key construction scattered across modules

69. **Per-fetcher `Cache::Key` schemas with no shared helper.** Each scheme builds `Cache::Key{<domain>, <attrs>}` ad hoc. Domains seen: `sourcePathToHash` (`fetch-to-store.cc`), `gitLastModified`/`gitRevCount` (`git.cc`), `gitRevToTreeHash`/`gitRevToLastModified` (`github.cc`), `treeHashToNarHash` (`git-utils.cc`), `hgRefToRev`/`hgRev` (`mercurial.cc`), `tarball`/`file` (`tarball.cc`). The `Cache` class is shared but each scheme picks its own attribute schema.
    - verified/14-libfetchers.md
70. **`makeSourcePathToHashCacheKey` is called from three places with slightly different shapes.** From `Input::getAccessorUnchecked`, `PathInputScheme::getAccessor`, and `fetch-to-store.cc`. They could share a helper.
    - verified/14-libfetchers.md
71. **NarInfoDiskCache key plumbing.** `lookupNarInfo`/`upsertNarInfo`/`upsertAbsentNarInfo` and `lookupRealisation`/`upsertRealisation`/`upsertAbsentRealisation` follow identical "TTL + present-bit + reconstruct" patterns. Could share a generic "TTL-cached lookup" base.
    - verified/08-libstore-remote.md

### Other distinct candidates

72. **`getDefaultFlakeAttrPaths` / `getDefaultFlakeAttrPathPrefixes` is copy-pasted across modern commands.** The `apps.<system>.default` + `defaultApp.<system>` shape appears verbatim in `CmdRun` and `CmdBundle`; `Common` (develop) has the analogous `devShells.<system>.default` + `devShell.<system>`; `MixFormatter` returns `formatter.<system>`; `CmdSearch` returns `packages.<system>` + `legacyPackages.<system>`. Each command also re-walks `SourceExprCommand::getDefaultFlakeAttrPaths()` to merge in base prefixes. A small `MixFlakeAttrPaths` helper would deduplicate.
    - verified/17-nix-modern-1.md
73. **JSON-vs-text dual output paths in commands.** A dozen commands (`CmdPathInfo`, `CmdFlakeMetadata`, `CmdFlakeShow`, `CmdFlakePrefetch`, `CmdFlakeArchive`, `CmdRealisationInfo`, `CmdConfigShow`, `CmdStorePrefetchFile`, `CmdProfileList`, `CmdSearch`, `CmdEval`, `CmdBuild`) all branch on `if (json) { printJSON(...) } else { logger->cout(...) }` with identical surrounding control flow.
    - verified/17-nix-modern-1.md
74. **GC dispatch logic duplicated three times.** `nix-store.cc opGC`, `nix-collect-garbage.cc main_nix_collect_garbage`, and `store-gc.cc CmdStoreGC::run` all open a `GcStore`, set `pathsToDelete = GCOptions::WholeStore{}`, wrap `collectGarbage` in `Finally`. The result-printing differs (always `printFreed` vs path-by-path).
    - verified/18-nix-modern-2-legacy.md
75. **Three NAR streaming entry points.** `CmdDumpPath::run` (`store dump-path`), `CmdDumpPath2::run` (`nar pack`), `nix-store.cc opDump`. The first two route through `dump-path.cc:getNarSink()`; the third constructs the `FdSink` directly and skips the TTY check. All three then call `narFromPath` or `dumpPath`.
    - verified/18-nix-modern-2-legacy.md
76. **Closure-walk helpers (BFS over `references`).** `nix-store/dotgraph.cc` and `nix-store/graphml.cc` both implement the same `StorePathSet workList`/`doneSet` BFS over `references`. They differ only in edge-direction and per-node emission. A shared `walkClosure(start, visit)` helper would consolidate.
    - verified/18-nix-modern-2-legacy.md
77. **Eval-cache release before `exec*` is duplicated four times.** `CmdRun::run`, `CmdDevelop::run`, `CmdShell::run`, `CmdFormatterRun::run` each call `state->evalCaches.clear()` immediately before exec'ing out of the process; the comment is identical at all four sites.
    - verified/17-nix-modern-1.md
78. **Lock-file walks in libflake.** `LockFile::isUnlocked`, `LockFile::getAllInputs`, `doFind` each implement a custom DFS over `Node::inputs` with their own visited-set; only `getAllInputs` is reused. A shared `forEachNode`/`forEachReachableEdge` helper would simplify all three.
    - verified/15-libflake-libmain.md
79. **Per-fetcher attrset-iteration with `if (n == "x") ... else if (n == "y") ... else error`.** Every fetcher primop iterates `*args[0]->attrs()` with this chain. `fetchTree`, `fetchClosure`, `fetchMercurial`, `fetch` are ripe for a helper that takes a `{ name → handler }` table and yields a uniform "unsupported argument" error.
    - verified/13-libexpr-primops.md
80. **`logFD` Setting<int> on Unix vs plain `Descriptor logFD` on Windows.** In `LegacySSHStoreConfig`. Inconsistent; the Windows side bypasses the settings system entirely.
    - verified/08-libstore-remote.md
81. **`Pid` holds `pid_t` on Unix vs `AutoCloseFD` on Windows.** Mostly fine, but the `release()` method exists only on Unix; `setSeparatePG`/`setKillSignal`/`setKillTimeout` are Unix-only; `wait`'s `allowInterrupts` is unused on Windows.
    - verified/04-libutil-misc.md
82. **`AutoUserLock`/`SimpleUserLock` `acquire` skeletons.** Both implementations open a per-slot lock file, try non-blocking exclusive lock via `lockFile(ltWrite, false)`, populate the lock object on success. The lock-acquisition skeleton could be shared.
    - verified/07-libstore-local.md
83. **`/nix/store` GC roots and runtime roots have three layers of similar logic.** `local-gc.cc::findRuntimeRootsUnchecked`, `gc.cc::requestRuntimeRoots`, and `gc.cc::LocalStore::findRuntimeRoots` form three layers; the first synthesises roots from `/proc` (or `lsof`), the second reads them from a Unix-domain socket, the third dispatches between the two. The `Roots` typedef and the file-local `UncheckedRoots` map use different key types (`StorePath` vs `std::string`).
    - verified/07-libstore-local.md
84. **`narHash` / `references` parser duplicates between `path-info.cc` JSON and `nar-info.cc` text.** Both reconstruct the same `UnkeyedValidPathInfo` fields with their own per-format error handling.
    - verified/05-libstore-core.md, verified/08-libstore-remote.md
85. **Setting<T> serialisation specialisations duplicated for `StoreReference`.** `BaseSetting<StoreReference>::parse`/`to_string`, `BaseSetting<std::vector<StoreReference>>::parse`/`to_string`/`appendOrSet`, `BaseSetting<std::set<StoreReference>>::parse`/`to_string`/`appendOrSet`. Three near-cut-and-pasted families.
    - verified/07-libstore-local.md
86. **`OnStartup` lambda registration pattern.** Used in libfetchers (`OnStartup([] { registerInputScheme(...) })`), libstore (`RegisterStoreImplementation<TConfig>` with a static instance), libcmd (`RegisterCommand` static instance), libexpr (`RegisterPrimOp` static instance), libstore/build (`RegisterBuiltinBuilder`). Each registry is a Meyers singleton with a different signature. A shared `Registry<Key, Factory>` template would consolidate.
    - verified/04-libutil-misc.md, verified/05-libstore-core.md, verified/14-libfetchers.md, verified/16-libcmd.md, verified/13-libexpr-primops.md
87. **`Forced vs lazy` value access in C bindings.** `nix_get_list_byidx{,_lazy}`, `nix_get_attr_byname{,_lazy}`, `nix_get_attr_byidx{,_lazy}` are nearly-identical pairs differing only by the presence of `forceValue` and slight error-message variations. The duplication is the most obvious internal symmetry in the C ABI.
    - verified/19-c-bindings-misc.md
88. **`nix_<libname>_init` family.** Each library exposes a parallel idempotent init. Could be a single template macro; currently each is a hand-rolled wrapper.
    - verified/19-c-bindings-misc.md

## Key invariants and guarantees

Cross-shard invariants worth preserving when refactoring. Each is documented
in the verified shard cited.

- **`StorePath` is the canonical store identifier.** Every cross-store comparison uses it. `StorePath::dummy` is `"ffffffffffffffffffffffffffffffff-x"`; `StorePath::MissingName = "x"` is the placeholder when only the hash is known. Constants: `StorePath::HashLen = 32` (160 bits), `StorePath::MaxPathLen = 211`. The character set is enforced by `nameRegexStr` in `path-regex.hh`.
  - verified/05-libstore-core.md
- **Hash algorithms.** SHA-256 is canonical for store paths and NAR hashes. MD5 / SHA-1 are legacy (still used for binary cache MD5 headers and git-tree hashing); BLAKE3 is gated on `Xp::BLAKE3Hashes`. `regularHashSize` returns 32/16/20/32/64 for SHA-256/MD5/SHA-1/SHA-256/SHA-512. `Hash::dummy` is a zero SHA-256.
  - verified/02-libutil-data.md
- **Wire protocol versioning.** `WorkerProto::Version` is `{Number, FeatureSet}` with **partial** ordering (subset relation on features); `WorkerProto::latest = 1.38` plus features `realisation-with-path-not-hash`, `delete-dead-specific-referrers`. `WorkerProto::minimum = 1.18`. `ServeProto::Version` is `{major, minor}` with **total** ordering and no `FeatureSet`; `ServeProto::latest = {2, 8}`. `CommonProto` has no version field at all (its serialisers are unconditional).
  - verified/09-libstore-protocol.md
- **Sandbox modes.** `enum SandboxMode { smEnabled, smRelaxed, smDisabled }`. Default is `smEnabled` on Linux/FreeBSD, `smDisabled` elsewhere. JSON serialises as `true`/`"relaxed"`/`false`. The corresponding setting is `sandbox` (with aliases `build-use-chroot`, `build-use-sandbox`).
  - verified/07-libstore-local.md
- **Build modes.** `enum BuildMode { bmNormal, bmRepair, bmCheck }`. Encoded as a single byte on the worker wire. `bmRepair` requires trust on the daemon side.
  - verified/05-libstore-core.md, verified/09-libstore-protocol.md
- **DerivationType variants.** `InputAddressed{deferred}`, `ContentAddressed{sandboxed, fixed}`, `Impure`. `BasicDerivation::type()` reduces per-output kinds (rejects mixed CA/non-CA, rejects multiple `CAFixed` outputs, rejects `CAFixed` named anything other than `"out"`). `DerivationType::isCA()` is true for `ContentAddressed` and `Impure`; `isFixed()` is `ContentAddressed{.fixed = true}` only; `isImpure()` is `Impure` only.
  - verified/06-libstore-derivations.md
- **`DerivationOutput` variants.** `InputAddressed{path}`, `CAFixed{ca}`, `CAFloating{method, hashAlgo}`, `Deferred{}`, `Impure{method, hashAlgo}`. The first two have known paths; the last three need realisation.
  - verified/06-libstore-derivations.md
- **`CloneableError<Derived, Base>` vs `MakeError(name, parent)`.** All exceptions descend from `BaseError`. `CloneableError<Derived, Base>` is the CRTP-shaped base that gives a definite `throwClone()` polymorphic copier; `MakeError(name, parent)` is the macro that emits a `class name : public CloneableError<name, parent>` declaration. Direct CRTP usage (`class X final : public CloneableError<X, BaseError>`) is reserved for exception types that need extra fields. Callers must rely on `throwClone()` to rethrow caught `BaseError`s with their dynamic type intact.
  - verified/03-libutil-runtime.md
- **`SourceAccessor::operator==` / `operator<=>` are derived from `number`.** Each `SourceAccessor` instance receives a unique increasing `number` from a process-global atomic; pointer-equality is not used for accessor comparison.
  - verified/01-libutil-io.md
- **`StorePathSet` is `std::set<StorePath>`; iteration order is the lexicographic order of the base name (stable across runs).** Used as a deterministic key for caches and topological sorts.
  - verified/05-libstore-core.md
- **`Bindings::iterator` performs a k-way merge across layers.** The layered "//" optimisation (capped at `maxLayers = 8`) means iteration is not a flat linear scan; ordering is preserved via heap merge. `BindingsBuilder::finishSizeIfNecessary` may collapse the chain when `bindingsUpdateLayerRhsSizeThreshold` (default 16 on 64-bit) is exceeded.
  - verified/11-libexpr-eval.md
- **`Counter` arithmetic short-circuits when `Counter::enabled` is false.** Set by `NIX_SHOW_STATS`. When disabled, every increment/decrement returns 0 without touching the atomic. Per-eval counters (`nrAvoided`, etc.) are gated on this.
  - verified/11-libexpr-eval.md
- **`SymbolTable` is append-only and concurrent.** Symbol IDs start at 1 (id 0 is the "unset" sentinel). Static symbols (`with`, `outPath`, etc.) are pre-allocated at compile time and replayed at runtime so that `EvalState::s.<name>` never allocates. `SymbolStr` is a `ChunkedVector` (chunks of 65536, capped at `numeric_limits<uint32_t>::max() / chunkSize`) with a front-side `boost::concurrent_flat_set` for interning.
  - verified/11-libexpr-eval.md
- **`ValidPathInfo::fingerprint` format is the binary-cache signature payload.** `1;<path>;<narHash Nix32>;<narSize>;<refs csv>`. Throws if `narSize == 0`. This is what every signature signs; deviation breaks binary-cache trust.
  - verified/05-libstore-core.md
- **`UnkeyedRealisation::fingerprint(key)` is JSON-serialised with `signatures` dropped.** Same scheme as `ValidPathInfo::fingerprint` but in JSON. Required for `Xp::CaDerivations` realisation signing.
  - verified/05-libstore-core.md
- **Schema version.** `nixSchemaVersion = 10` (in `local-store.hh`). Migrations after schema 10 use the named-migration mechanism (`SchemaMigrations` table) rather than the on-disk `schema` integer file. Currently named migrations: `20251017-ca-derivations`, `20260309-drop-redundant-indexreferrer`. Pre-10 migrations live inline in the `LocalStore` constructor.
  - verified/07-libstore-local.md
- **GC self-reference protection.** `DeleteSelfRefs` SQLite trigger fires `BEFORE DELETE ON ValidPaths` and removes any `(N, N)` rows in `Refs` so the `ON DELETE RESTRICT` foreign key on the `reference` column does not block deletion of a self-referential row.
  - verified/07-libstore-local.md
- **`mtimeStore = 1` is the canonical mtime for store paths.** Every regular file in a valid store path has its mtime canonicalised to 1 second after the epoch (Unix only). Permissions are masked to `0444` / `0555` (preserving exec bits for files and dir flag for directories).
  - verified/05-libstore-core.md
- **`LocalStore::optimiseStore` uses `linksDir` for hard-link deduplication.** Equal regular files (and on platforms where `CAN_LINK_SYMLINK`, equal symlinks) are hard-linked into `<store>/.links/` so the on-disk store has at most one copy per content hash.
  - verified/07-libstore-local.md
- **`drvHashes` is the global memoisation map for `hashDerivationModulo`.** A `boost::concurrent_flat_map` keyed on `StorePath` and storing `DrvHashModulo`. Required for repeatable derivation-hash computation across store invocations.
  - verified/06-libstore-derivations.md

## Methodology footer

This index was produced in three passes:

1. The codebase under `src/` was sharded into 19 logical groups; per-shard file lists live in `doc/inventory/shards/`.
2. Each shard was first catalogued by a Haiku subagent (raw inventories in `doc/inventory/raw/`), then verified and corrected by an Opus subagent against the source (final inventories in `doc/inventory/verified/`). Three of the Haiku runs and one of the Opus runs failed mid-stream and were retried; the verified outputs reflect a complete, source-checked snapshot.
3. This index was itself written by an agent that read all 19 verified shards. Any reader should treat it as a navigation tool and verify load-bearing claims (especially in section 5) against the verified shards, and ultimately against the source. The verified shards are the authoritative artefact; this index is a finder's guide, not a substitute.

