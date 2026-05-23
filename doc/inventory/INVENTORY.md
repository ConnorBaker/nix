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

The numbered, prioritised list of refactoring opportunities surfaced by the
per-shard verified docs and follow-up source audits lives in
[`candidates/`](candidates/), split into 25 themed section files with an
[index](candidates/README.md). Each candidate carries a validation verdict
(VALID / PARTIALLY VALID / INVALID / OBSOLETE) and an effort class against
the source as it exists today. The 221 per-shard candidates (1-221;
#218 was added during the V-pass integration documenting the
`Hash::dummy`/`StorePath::dummy` placeholder pattern, #219 was added
during post-cleanup adversarial review documenting the post-build-hook
timeout silent-drop bug, and #220/#221 were added during the May 2026
Pass B reuse review when the catalog gaps surfaced in the course of
landing #79 and #85) plus 44 cross-shard candidates (numbered N1-N44)
are grouped as:
wire/serialisation duplication, parallel store implementations, repeated
boilerplate, per-platform symmetry, inheritance chains worth flattening,
multi-implementation patterns, dead/stale code, duplicated parsers,
scattered cache keys, other distinct candidates, legacy CLI duplication,
libutil + libstore-core extras, libexpr extras, vestigial code / dead
workarounds / stdlib replacements, globals/settings architecture,
libstore/build deep audit, cross-cutting (platforms, headers, magic
numbers), daemon and protocol-dispatch deep audit, libexpr eval-core
(fetcher primops, lookup-path, JSON), libexpr eval-core (cache, attr-set,
profiler), libexpr eval-core (EvalState and Value), cross-shard patterns
(N1-N23), C-API surface duplication (Cluster E; N24-N25, N29),
test-infrastructure mirrors (Cluster F; N28, N39, N41), and
dispatch tables and schemas (Cluster G; N26, N30, N32-N38, N40, N42-N44,
plus other pass-2 candidates). Each candidate carries a
`**Validation:**` paragraph with the verdict, effort class, and any
factual corrections folded in from the verification pass.

The candidates README also documents the **architectural debt themes**
A1-A7 (cross-cutting roadmap entries that the per-candidate work feeds
into), the **C++23/Boost uplift series** U1-U10 (a coherent staged
modernisation series with sequencing), and the **redundant
abstractions** R1-R10 (cases where 2+ in-tree abstractions solve
overlapping problems and the choice rule is undocumented).

The catalog is being actively worked. The current state — what's
queued, in flight, blocked, recently completed, plus the cleanup-branch
table and worktree layout — lives in [`STATUS.md`](STATUS.md). Reports
from past investigations (adversarial review passes, pattern-discovery
passes, follow-up agents, verification passes) are indexed in
[`review/00-INDEX.md`](review/00-INDEX.md). Operational rules for new
agents working on the catalog (the eight-rule evidentiary standard
derived from prior failure modes) live in
[`review/AGENT-CHARTER.md`](review/AGENT-CHARTER.md). A new agent
picking up this work should read in order: this file (INVENTORY.md) →
[`candidates/README.md`](candidates/README.md) →
[`STATUS.md`](STATUS.md) → [`review/00-INDEX.md`](review/00-INDEX.md) →
[`review/AGENT-CHARTER.md`](review/AGENT-CHARTER.md).

## Key invariants and guarantees

Cross-shard invariants worth preserving when refactoring. Each is documented
in the verified shard cited.

- **`StorePath` is the canonical store identifier.** Every cross-store comparison uses it. `StorePath::dummy` is `"ffffffffffffffffffffffffffffffff-x"`; `StorePath::MissingName = "x"` is the placeholder when only the hash is known. Constants: `StorePath::HashLen = 32` (160 bits), `StorePath::MaxPathLen = 211`. The character set is enforced by `nameRegexStr` in `path-regex.hh`.
  - verified/05-libstore-core.md
- **Hash algorithms.** SHA-256 is canonical for store paths and NAR hashes. MD5 / SHA-1 are legacy (still used for binary cache MD5 headers and git-tree hashing); BLAKE3 is gated on `Xp::BLAKE3Hashes`. `regularHashSize` returns 32/16/20/32/64 for BLAKE3/MD5/SHA-1/SHA-256/SHA-512. `Hash::dummy` is a zero SHA-256.
  - verified/02-libutil-data.md
- **Wire protocol versioning.** `WorkerProto::Version` is `{Number, FeatureSet}` with **partial** ordering (subset relation on features); `WorkerProto::latest = {1, 38}` plus features `realisation-with-path-not-hash`, `delete-dead-specific-referrers`. `WorkerProto::minimum = {1, 18}`. **Note:** the `PROTOCOL_VERSION` macro in `worker-protocol.hh` is `(1 << 8 | 39)` (bumped 2026-04 for new wire ops); `WorkerProto::latest` is the C++ struct constant used in feature-gated paths and remains at `{1, 38}` until the matching feature entry lands. The macro and the struct are genuinely two different version handles. `ServeProto::Version` is `{major, minor}` with **total** ordering and no `FeatureSet`; `SERVE_PROTOCOL_VERSION` macro is `(2 << 8 | 8)`; `ServeProto::latest = {2, 8}`. `CommonProto` has no version field at all (its serialisers are unconditional).
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

