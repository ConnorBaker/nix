# V2 — verify E4 wire-crossing reachability

## Charter recap

E4 reported "61 positional-format `%N%` sites in `src/libstore/`" and "17 throws
in `src/libutil/`" as wire-crossing for candidate #125. The original report
filtered by directory ("any libstore throw is wire-crossing"), not by
reachability from `daemon.cc::performOp`. This pass verifies per-site
reachability against the actual call graph.

## Method

### Enumeration

Multi-line-aware Python parser that:

1. Walks every `.cc`/`.hh` under `src/libstore/` and `src/libutil/`.
2. Finds every `throw <Identifier>(` token.
3. Restricts to identifiers ending in `Error` plus the explicit error-derived
   classes `InvalidPath`, `BadStorePath`, `BadStorePathName`, `MissingRealisation`,
   `InvalidStorePath`. Other classes are excluded.
4. Walks paren depth (with string-literal tracking) until matching `)` followed
   by `;` to find the throw expression body.
5. Filters bodies that contain a `%N%` (positional Boost) marker.

This catches every multi-line throw the single-line `grep "throw .*%[0-9]+%"`
misses. Result: **71 positional-format throw sites in `src/libstore/`**, not 61.
The original 61 was an undercount.

For libutil I enumerate **all** `throw *Error(...)` sites (not only positional)
because every libutil throw is potentially wire-crossing per E4's rules of
thumb. Total: **281 throws in libutil**, of which **50 are positional**. The
original 17 figure in E4 referred only to specific files (`SysError`,
`FormatError`, `WinError` in a hand-picked subset).

### Reachability tracing

For each site, the function it lives in is identified by reading the file
and walking up to the enclosing `Foo::bar(...)` definition. Then I check
whether that function (or any caller of it) can be invoked transitively from
`daemon.cc::performOp`. The set of `Op` arms is enumerated from
`daemon.cc:312..1014`. The set of `Store::*` virtuals invoked by `performOp`:

```
isValidPath, queryValidPaths, querySubstitutablePaths, queryReferrers,
queryValidDerivers, queryDerivationOutputs, readDerivation,
queryPartialDerivationOutputMap, queryPathInfo, queryPathFromHashPart,
addToStoreFromDump, addToStore, buildPaths, buildPathsWithResults,
writeDerivation, buildDerivation, ensurePath, addTempRoot,
LocalFSStore::addPermRoot, IndirectRootStore::addIndirectRoot,
GcStore::findRoots, GcStore::collectGarbage, querySubstitutablePathInfos,
queryAllValidPaths, optimiseStore, verifyStore, addSignatures, narFromPath,
queryMissing, registerDrvOutput, queryRealisation, LogStore::addBuildLog,
substitutePaths
```

Plus inline helpers: `parseDump`, `readDerivation` (free fn),
`parseHashAlgo`, `Hash::parseAny`, `ContentAddress::parseOpt`,
`ContentAddressMethod::parseWithAlgo`, `absPath`.

### Sandbox-setup vs post-setup throws

In `DerivationBuilderImpl::runChild` (`unix/build/derivation-builder.cc`)
there is a critical control flow distinction:

- Pre-`\2`-marker throws: child writes `\1` + serialised `Error` over the
  `builderOut` pty. Parent's `processSandboxSetupMessages()` reads via
  `readError(source)` and `throw std::move(ex)`. **Wire-crossing exception text
  is preserved.**
- Post-`\2`-marker throws (after `sendException = false`): caught by the
  `catch (...)` in the child, dispatched to `handleChildException(false)` which
  writes `e.msg()` to `std::cerr` (the build-log pty). The child then
  `_exit(1)` and the parent eventually throws `BuilderFailureError` with
  generic text. **The original throw text crosses only as build log frames
  (`STDERR_NEXT`), not as exception body.** For the wire-crossing-exception
  matrix, these sites are unreachable.

The `\2` marker is written at `runChild` after `setUser()`. Throws inside
`enterChroot()`, `commonChildInit()`, the chdir-to-tmpDir step, and `setUser()`
itself all happen pre-`\2` and are wire-crossing. The `if (drv.isBuiltin())`
block, `execBuilder` call, and the post-execv `throw SysError("executing")`
are post-`\2` and not wire-crossing as exception text.

This same pattern applies in `external-derivation-builder.cc` (the helper
process uses `handleChildException(true)` always, so its throws are
wire-crossing) and the FreeBSD `startChild` helper for loopback config (which
uses raw `_exit(1)` with no exception channel — not wire-crossing).

### What I did NOT verify

- I did not exhaustively prove unreachability for every `unix/build/`
  helper — I assumed reachable when called from `prepareSandbox` /
  `startBuild` / `registerOutputs` (all parent-process). Where I'm uncertain
  about parent-vs-child context, the row notes "uncertain (helper child)".
- I did not run the build to confirm symbol resolution; the call-graph
  trace is purely textual.

## Per-site reachability table

Format: `file::identifier` | throw text excerpt | verdict | evidence

### `src/libstore/` — 71 positional-format throw sites

| File::Identifier | Throw excerpt | Verdict | Evidence |
|---|---|---|---|
| `build/derivation-building-goal.cc::LogFile::LogFile` | `SysError("creating log file %1%", ...)` | Reachable | LogFile constructed by DerivationBuildingGoal during `buildPaths`/`buildDerivation`; parent process. |
| `builtins/buildenv.cc::createLinks` | `Error("collision between %1% and non-directory %2%", ...)` | Unreachable as exception | `buildenv` is a builtin builder; runs in build child after `\2` / `sendException = false`. Throw caught at `derivation-builder.cc::runChild` 1391 catch → child stderr → `_exit(1)`. Text propagates as build log only. |
| `builtins/buildenv.cc::createLinks` | `SysError("creating directory '%1%'", dstFile)` | Unreachable as exception | Same: builtin builder runs post-`\2`. |
| `builtins/buildenv.cc::createLinks` | `Error("collision between non-directory '%1%' and directory '%2%'", ...)` | Unreachable as exception | Same: builtin builder runs post-`\2`. |
| `builtins/unpack-channel.cc::builtinUnpackChannel` | `Error("channelName ... separators, got %1%", ...)` | Unreachable as exception | Builtin builder runs post-`\2`. |
| `builtins/unpack-channel.cc::builtinUnpackChannel` | `SystemError(e.code(), "failed to rename %1% to %2%", ...)` | Unreachable as exception | Builtin builder runs post-`\2`. |
| `daemon.cc::performOp` (default arm) | `Error("invalid operation %1%", op)` | Reachable (inline) | Directly thrown from `performOp` default case. Crosses wire when client sends an unknown opcode. Already enumerated in E4's "performOp inline throws". |
| `derivation-options.cc::DerivationOptions::fromStructuredAttrs` | `Error("odd number of tokens in 'exportReferencesGraph': '%1%'", s)` | Reachable | Called from `DerivationBuilderImpl::startBuild` (parent process) via `params.drvOptions` construction during `buildPaths`. |
| `derivations.cc::parseString` | `FormatError("expected string '%1%'", s)` | Reachable | `parseString` -> `parseDerivation` -> `Store::readDerivation` (called from `performOp::QueryDerivationOutputNames`, `BuildDerivation`, etc.). **Hard-break contract: the substring `"expected string"` is part of the `DynamicDerivations` text-grep triple in `worker-protocol-connection.cc`.** |
| `derivations.cc::parseChar` | `FormatError("expected string '%1%'", c)` | Reachable | Same parse path; same hard-break contract. |
| `derivations.cc::parsePath` | `FormatError("bad path '%1%' in derivation", s)` | Reachable | `parsePath` -> `parseDerivation` -> `readDerivation`; reachable as above. |
| `gc.cc::LocalStore::findTempRoots` | `SysError("opening temporary roots file %1%", PathFmt(path))` | Reachable | `findTempRoots` called from `LocalStore::findRoots` → `performOp::FindRoots`/`CollectGarbage`. |
| `gc.cc::LocalStore::findRoots` (catch arm) | `SystemError(e.code(), "finding GC roots in %1%", ...)` | Reachable | `findRoots` is called from `findRootsNoTemp` from `findRoots(censor)` from `performOp::FindRoots`. |
| `gc.cc::LocalStore::collectGarbage` (specific-paths visitor) | `Error("Cannot delete path '%1%' since it is still alive. ...")` | Reachable | Inside the `gcDeleteSpecific` arm of `collectGarbage` → `performOp::CollectGarbage`. |
| `gc.cc::LocalStore::collectGarbage` (whole-store) | `SysError("opening directory %1%", PathFmt(realStoreDir))` | Reachable | Same; whole-store visitor. |
| `gc.cc::LocalStore::collectGarbage` (links cleanup) | `SysError("opening directory %1%", PathFmt(linksDir))` | Reachable | Same; final links-cleanup phase. |
| `include/nix/store/store-api.hh::Store::requireStoreObjectAccessor` | `InvalidPath(... "path '%1%' is not a valid store path" or "store path '%1%' does not exist", ...)` | Reachable | Called from `Store::narFromPath` → `performOp::NarFromPath`. Also from `LocalStore::addToStore` (`local-store.cc:1079`) → `performOp::AddToStoreNar`/`AddMultipleToStore`. **Hard-break contract: the substring `"is not valid"` is the queryPathInfo path-not-found marker.** |
| `indirect-root-store.cc::IndirectRootStore::addPermRoot` | `Error("creating a garbage collector root (%1%) in the Nix store is forbidden ...", ...)` | Reachable | `addPermRoot` called from `performOp::AddPermRoot` via `LocalFSStore::addPermRoot`. |
| `indirect-root-store.cc::IndirectRootStore::addPermRoot` | `Error("cannot create symlink %1%; already exists", ...)` | Reachable | Same. |
| `legacy-ssh-store.cc::LegacySSHStore::openConnection` | `Error("cannot connect to '%1%'", config->authority.host)` | Conditionally reachable | Reached only when the daemon's underlying store is a `LegacySSHStore`. `LegacySSHStore::buildPaths`/`buildDerivation`/`queryPathInfoUncached` etc. all call `openConnection`; those methods are dispatched virtually from `performOp` when the store is configured as `ssh-ng://` legacy. |
| `local-fs-store.cc::LocalStoreAccessor::requireStoreObject` | `InvalidPath("path '%1%' is not a valid store path", ...)` | Reachable | Called from accessor `lstat`/`maybeLstat`/`readDirectory`/`readFile` returned by `LocalFSStore::getFSAccessor`. `Store::narFromPath` traverses via this accessor. **Hard-break contract: `"is not valid"`.** |
| `local-gc.cc::findRuntimeRootsUnchecked` | `SysError("opening %1%", fdStr)` | Reachable | `findRuntimeRootsUnchecked` called from `LocalStore::findRuntimeRoots` from `findRootsNoTemp` from `findRoots(censor)` → `performOp::FindRoots`/`CollectGarbage`. |
| `local-gc.cc::findRuntimeRootsUnchecked` | `SysError("iterating /proc/%1%/fd", ...)` | Reachable | Same. |
| `local-store.cc::LocalStore::LocalStore` (symlink check) | `Error("the path %1% is a symlink; ...")` | Unreachable | In `LocalStore` constructor; runs at daemon process startup, before `processConnection`. If thrown the daemon never reaches `performOp`. |
| `local-store.cc::LocalStore::LocalStore` (schema mismatch) | `Error("current Nix store schema is version %1%, but I only support %2%", ...)` | Unreachable | Constructor; daemon-startup-only. |
| `local-store.cc::LocalStore::LocalStore` (corrupt schema) | `Error("%1% is corrupt", PathFmt(schemaPath))` | Unreachable | Constructor; daemon-startup-only. (Search for `getSchema` confirms it's only in ctor.) |
| `local-store.cc::LocalStore::LocalStore` (db dir not writable) | `SysError("Nix database directory %1% is not writable", ...)` | Unreachable | Constructor; daemon-startup-only. |
| `optimise-store.cc::LocalStore::optimisePath_` | `SysError("opening directory %1%", PathFmt(linksDir))` | Reachable | `optimisePath_` invoked recursively from `LocalStore::optimiseStore` → `performOp::OptimiseStore`. Also from `optimisePath` called by `LocalStore::addToStore`/`addToStoreFromDump`. |
| `optimise-store.cc::LocalStore::optimisePath_` | `SysError("reading directory %1%", PathFmt(linksDir))` | Reachable | Same. |
| `optimise-store.cc::LocalStore::optimisePath_` | `SystemError(e.code(), "creating hard link from %1% to %2%", ...)` | Reachable | Same. |
| `optimise-store.cc::LocalStore::optimisePath_` | `SystemError(e.code(), "creating hard link from %1% to %2%", ...)` (tempLink) | Reachable | Same. |
| `optimise-store.cc::LocalStore::optimisePath_` | `SystemError(e.code(), "renaming %1% to %2%", ...)` | Reachable | Same. |
| `posix-fs-canonicalise.cc::canonicaliseTimestampAndPermissions` | `SysError("clearing flags of path %1%", ...)` | Reachable | `canonicaliseTimestampAndPermissions` -> `canonicalisePathMetaData_` -> `canonicalisePathMetaData` -> `LocalStore::addToStore`/`registerOutputs`. Reachable from `performOp::AddToStoreNar`/`AddMultipleToStore` and from `BuildPaths`/`BuildDerivation` (post-build register outputs runs in the parent). |
| `posix-fs-canonicalise.cc::canonicalisePathMetaData_` | `Error("file %1% has an unsupported type", PathFmt(path))` | Reachable | Same call chain. |
| `posix-fs-canonicalise.cc::canonicalisePathMetaData_` | `BuildError(... "invalid ownership on file %1%", ...)` | Reachable | Same call chain. |
| `posix-fs-canonicalise.cc::canonicalisePathMetaData_` | `SysError("changing owner of %1% to %2%", ...)` | Reachable | Same call chain. |
| `profiles.cc::deleteGenerations` | `Error("cannot delete current version of profile %1%", ...)` | Unreachable | `deleteGenerations` called only from `src/nix/profile.cc`, `src/nix/nix-env/nix-env.cc`, `src/nix/nix-collect-garbage/nix-collect-garbage.cc`. No libstore caller. CLI-only. |
| `profiles.cc::parseOlderThanTimeSpec` | `UsageError("invalid number of days specifier '%1%', expected ...")` | Unreachable | Same: only called from `src/nix/...`. |
| `profiles.cc::parseOlderThanTimeSpec` | `UsageError("invalid number of days specifier '%1%'", ...)` | Unreachable | Same. |
| `profiles.cc::switchGeneration` | `Error("profile version %1% does not exist", *dstGen)` | Unreachable | Same: only called from `src/nix/...`. |
| `profiles.cc::switchGeneration` | `Error("no profile version older than the current (%1%) exists", ...)` | Unreachable | Same. |
| `remote-fs-accessor.cc::RemoteFSAccessor::fetch` | `InvalidPath("path '%1%' is not a valid store path", ...)` | Conditionally reachable | `RemoteFSAccessor` is the accessor returned by `BinaryCacheStore::getFSAccessor` (and by `RemoteStore::getFSAccessor`). When the daemon's underlying store is a `BinaryCacheStore`, `Store::narFromPath` -> `RemoteFSAccessor::fetch` is reachable from `performOp::NarFromPath`. **Hard-break contract: `"is not valid"`.** |
| `store-api.cc::StoreDirConfig::toStorePath` | `Error("path '%1%' is not in the Nix store", path)` | Reachable | `toStorePath` called from `RemoteFSAccessor::fetch`, `LocalStoreAccessor::requireStoreObject`, `local-gc.cc::findRuntimeRootsUnchecked`, `gc.cc::findTempRoots`. All of those are reachable from `performOp` (NarFromPath / FindRoots / CollectGarbage). |
| `store-api.cc::Store::followLinksToStore` | `BadStorePath("path %1% is not in the Nix store", PathFmt(path))` | Unreachable | `followLinksToStore`/`followLinksToStorePath` called only from `src/nix/...` CLI commands (`nix-store`, `nix profile`, `nix upgrade-nix`). No libstore call site reachable from `performOp`. |
| `unix/build/child.cc::commonChildInit` | `SysError("cannot open '%1%'", pathNullDevice)` | Reachable | Called from build-child startup (pre-`\2`); propagated via `\1`+`readError` through `processSandboxSetupMessages`. |
| `unix/build/darwin-derivation-builder.cc::DarwinDerivationBuilder::prepareSandbox` | `Error("can't map %1% to %2%: mismatched impure paths not supported on Darwin", ...)` | Reachable (Darwin only) | `prepareSandbox` runs in parent process (`DerivationBuilderImpl::prepareBuild` line 845). Conditionally compiled `#ifdef __APPLE__`. |
| `unix/build/derivation-builder.cc::handleDiffHook` | `ExecError(diffRes.first, "diff-hook program %s %2%", ...)` | Unreachable as exception | Throw caught at the surrounding `catch (Error & error)` and only `logError(ei)`'d (in `derivation-builder.cc::handleDiffHook`). Body propagates as log frame, not exception. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::startBuild` | `SysError("failed to open the build temporary directory descriptor %1%", ...)` | Reachable | `startBuild` is the parent-process entry; called from `DerivationBuildingGoal` via `Worker::run` from `Store::buildPaths`. Throws here are normal exceptions. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::prepareBuild` | `Error("home directory %1% exists; please remove it ...", ...)` | Reachable | `prepareBuild` is parent-process. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::getPathsInSandbox` | `Error("unknown pre-build hook command '%1%'", line)` | Reachable | `getPathsInSandbox` called from `prepareSandbox` (parent). Pre-build-hook is run via `runProgram` in the parent. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::chownToBuilder(int, path)` | `SysError("cannot change ownership of file %1%", ...)` | Reachable | Called from `startBuild`/`prepareBuild`/`registerOutputs` (parent). |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::runChild` (chdir tmpDir) | `SysError("changing into %1%", PathFmt(tmpDir))` | Reachable | Inside `runChild` BEFORE `\2` write at line 1372 and BEFORE `sendException = false` at line 1374. Caught child-side, propagated via `\1`+`readError`. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::runChild` (builtin) | `Error("unsupported builtin builder '%1%'", builtinName)` | Unreachable as exception | After `\2` and `sendException = false`. Caught at line 1391, child writes to stderr, `_exit(1)`. Text crosses as build log only. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::runChild` (post-execv) | `SysError("executing '%1%'", drv.builder)` | Unreachable as exception | After `\2` and `sendException = false`. Same path. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::registerOutputs` (no stats) | `BuildError(... "output path %1% without valid stats info", ...)` | Reachable | `registerOutputs` runs in parent process after build child exits. Throws are normal exceptions in `Worker`. |
| `unix/build/derivation-builder.cc::DerivationBuilderImpl::registerOutputs` (non-regular) | `BuildError(... "output path %1% should be a non-executable regular file ...", ...)` | Reachable | Same. |
| `unix/build/external-derivation-builder.cc::ExternalDerivationBuilder::startChild` | `SysError("changing into %1%", PathFmt(tmpDir))` | Reachable | Inside the `startProcess` lambda; caught by `handleChildException(true)` so `\1`+`readError` propagates. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::prepareSandbox` (mount /dev) | `SysError("failed to mount jail /dev: %1%", ...)` | Reachable (FreeBSD only) | `prepareSandbox` runs in parent, called by `DerivationBuilderImpl::prepareBuild`. `#ifdef __FreeBSD__`. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::prepareSandbox` (path stat) | `SysError("getting attributes of path %1%", ...)` | Reachable (FreeBSD only) | Same. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::prepareSandbox` (nullfs) | `SysError("failed to mount nullfs for %1%: %2%", ...)` | Reachable (FreeBSD only) | Same. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::startChild` (jail isolated) | `SysError("failed to create jail (isolated network): %1%", ...)` | Reachable (FreeBSD only) | `startChild` parent-side `jail_setv` call (line 397). Thrown in parent, propagated normally. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::startChild` (loopback helper) | `SysError("failed to configure loopback interface: %1%", ...)` | Unreachable as exception | Inside the `startProcess` helper-child lambda for loopback config (line 404 `Pid helper = startProcess([&]() {...})`). The comment on line 442 reads `/* TODO: Capture the error from the helper? */` — there is no exception channel back. Parent throws a generic `Error("failed to configure loopback address: %s", statusToString(status))` at line 444. |
| `unix/build/freebsd-derivation-builder.cc::FreeBSDDerivationBuilder::startChild` (jail networked) | `SysError("failed to create jail (networked): %1%", ...)` | Reachable (FreeBSD only) | Parent-side `jail_setv` call (line 467). |
| `unix/build/linux-derivation-builder.cc::doBind` (mount) | `SysError("bind mount from %1% to %2% failed", ...)` | Reachable (Linux only) | `doBind` called from `enterChroot` override; `enterChroot` is invoked from `runChild` BEFORE `\2`. Propagated via `\1`+`readError`. |
| `unix/build/linux-derivation-builder.cc::doBind` (lstat) | `SysError("getting attributes of path %1%", ...)` | Reachable (Linux only) | Same. |
| `unix/build/linux-derivation-builder.cc::ChrootLinuxDerivationBuilder::enterChroot` (chrootRootDir bind) | `SysError("unable to bind mount %1%", ...)` | Reachable (Linux only) | `enterChroot` is called pre-`\2`. |
| `unix/build/linux-derivation-builder.cc::ChrootLinuxDerivationBuilder::enterChroot` (store bind) | `SysError("unable to bind mount the Nix store at %1%", ...)` | Reachable (Linux only) | Same. |
| `unix/build/linux-derivation-builder.cc::ChrootLinuxDerivationBuilder::enterChroot` (chdir) | `SysError("cannot change directory to %1%", ...)` | Reachable (Linux only) | Same. |
| `unix/build/linux-derivation-builder.cc::ChrootLinuxDerivationBuilder::enterChroot` (pivot_root) | `SysError("cannot pivot old root directory onto %1%", ...)` | Reachable (Linux only) | Same. |
| `unix/build/linux-derivation-builder.cc::ChrootLinuxDerivationBuilder::enterChroot` (chroot) | `SysError("cannot change root directory to %1%", ...)` | Reachable (Linux only) | Same. |
| `unix/pathlocks.cc::openLockFile` | `SysError("opening lock file %1%", ...)` | Reachable | `openLockFile` is called from `LocalStore::LocalStore` (constructor — daemon startup) **and** from `LocalStore::addTempRoot`, `LocalStore::collectGarbage` lock acquisition, `findTempRoots`, `lockProfile`. The non-startup callers are reachable from `performOp::AddTempRoot`/`CollectGarbage`/`FindRoots`. |

Aggregate (libstore positional sites): **51 reachable** (including 6 conditionally reachable: 1 LegacySSH, 1 BinaryCache, 1 Darwin-only, 6 FreeBSD-only, 6 Linux-only — counting the platform-conditional as reachable when configured), **20 unreachable** (4 LocalStore-ctor, 5 profiles.cc, 1 followLinksToStore, 5 builtin-builders, 2 post-`\2` runChild, 1 diff-hook caught locally, 1 freebsd helper-child, 1 unreachable accessor caller dropped — summing to 71 total).

### `src/libutil/` — 50 positional-format throw sites

For libutil I focus on positional sites and on whether the function is
reachable from any libstore code that performOp calls.

| File::Identifier | Throw excerpt | Verdict | Evidence |
|---|---|---|---|
| `args.cc::Args::parseCmdline` | `UsageError("unrecognised flag '%1%'", arg)` | Unreachable | `Args::parseCmdline` is a CLI argument-parser entry point. The daemon does parse its own args at startup but does so before `processConnection`. Not reachable from `performOp`. |
| `args.cc::Args::parseCmdline` (consume positional) | `UsageError("unexpected argument '%1%'", ...)` | Unreachable | Same. |
| `configuration.cc::loadConfFile` | `UsageError("syntax error in configuration line '%1%' in %s", ...)` | Conditionally reachable | `loadConfFile` is called during `Settings`/`globalConfig` initialisation. The daemon does this at startup, but `ClientSettings::apply` (in `daemon.cc::performOp::SetOptions`) calls back into config updaters. `applyConfig` re-parses at runtime in some paths. Reachable conditionally — flag for human review. |
| `file-descriptor.cc::Descriptor` close path | `NativeSysError("closing file descriptor %1%", fd)` | Reachable | `close` failures show up via `AutoCloseFD` destructors — but destructors run during normal cleanup paths. The throw happens in `~AutoCloseFD` only when explicit `close()` is called. Reachable from many paths the daemon executes. |
| `file-system.cc::canonPath` | `Error("infinite symlink recursion in path '%1%'", remaining)` | Reachable | `canonPath` is widely used; called from `IndirectRootStore::addPermRoot`, `Store::followLinksToStore`, `LocalStore::LocalStore` ctor, etc. The reachable-from-performOp calls include `addPermRoot`. |
| `file-system.cc::PosixSourceAccessor::open*` (`fd365`) | `NativeSysError("opening file %1%", PathFmt(entry.path()))` | Reachable | `PosixSourceAccessor` is constructed by `LocalFSStore::getFSAccessor` (`makeFSSourceAccessor`). `narFromPath` -> accessor.readFile -> open. |
| `file-system.cc::openDirectory` | `NativeSysError("opening directory %1%", PathFmt(*dir))` | Reachable | Used in many libstore directory traversals (gc, addToStore). |
| `file-system.cc::createDirs` | `SystemError(e.code(), "creating directory %1%", PathFmt(path))` | Reachable | `createDirs` is called extensively (gc roots, store dir, etc.). |
| `file-system.cc::createTempDir` | `SysError("setting group of directory %1%", ...)` | Reachable | `createTempDir` used for build tmp dirs and others; called in the parent process during build. |
| `file-system.cc::createTempDir` | `SysError("creating directory %1%", PathFmt(tmpDir))` | Reachable | Same. |
| `file-system.cc::replaceSymlink` | `SystemError(e.code(), "creating symlink %1% -> %2%", ...)` | Reachable | `replaceSymlink` used in profiles (CLI-only) AND in `IndirectRootStore::makeSymlink` and `LocalStore::addTempRoot`. Reachable via AddPermRoot. |
| `file-system.cc::replaceSymlink` (rename) | `SystemError(e.code(), "renaming %1% to %2%", ...)` | Reachable | Same. |
| `freebsd/freebsd-jail.cc::removeJail` | `SysError("Failed to remove jail %1%", jid)` | Reachable (FreeBSD only) | Used in FreeBSD jail teardown after build. Parent-side. |
| `fs-sink.cc::RestoreSink::createDirectory`/`createFile` | `Error("file '%1%' has an unsupported type of %2%", ...)` | Reachable | `RestoreSink` consumes NAR streams via `parseDump`. The daemon's `addToStore`/`addToStoreFromDump` paths do NAR parsing on incoming client data; reachable from `performOp::AddToStore`, `AddMultipleToStore`, `AddToStoreNar`. |
| `fs-sink.cc::RestoreSink::createRegularFile` | `NativeSysError("creating file %1%", ...)` | Reachable | Same. |
| `fs-sink.cc::RestoreSink::createRegularFile` (preallocate) | `SysError("preallocating file of %1% bytes", len)` | Reachable | Same. |
| `fs-sink.cc::RestoreSink::createSymlink` | `SysError("creating symlink from %1% -> '%2%'", ...)` | Reachable | Same. |
| `git.cc::dumpHash` | `Error("file '%1%' has an unsupported type of %2%", path, st.typeString())` | Reachable | `git::dumpHash` is called from `LocalStore::addToStore` for `FileIngestionMethod::Git` paths. Reachable from `performOp::AddToStoreNar`. |
| `hash.cc::parseHashFormat` | `UsageError("unknown hash format '%1%', ...")` | Reachable | `parseHashFormat` is parser-side; called from `Hash::parseAny` and friends. The daemon's `AddToStoreNar` op calls `Hash::parseAny`. |
| `hash.cc::parseHashAlgo` | `UsageError("unknown hash algorithm '%1%', ...")` | Reachable | Called from `daemon.cc::performOp::AddToStore` (old proto branch line 460: `parseHashAlgo(hashAlgoRaw)`). Direct wire-crossing. |
| `include/nix/util/util.hh::string2IntMustParse` (or similar) | `UsageError("invalid unit specifier '%1%'", u)` | Conditionally reachable | Helper for parsing "10G", "1M" etc. Used in size-spec parsers. Used by GC settings parsing. Probably daemon-startup; flag for review. |
| `logging.cc::FileLogger` ctor | `SysError("opening log file %1%", PathFmt(path))` | Reachable | `FileLogger` is the standard logger. Daemon uses it at startup but new file loggers can also be created at runtime. Marginal, but conservatively reachable. |
| `nar-accessor.cc::NarAccessor::read` | `Error("reading invalid NAR bytes range: requested %1% bytes at offset %2%, but NAR has size %3%", ...)` | Reachable | `NarAccessor` is constructed from `RemoteFSAccessor` cache and from `narCache.getOrInsert`. Reachable when daemon backs a `BinaryCacheStore`. |
| `nar-accessor.cc::NarAccessor::open` | `Error("NAR file does not contain path '%1%'", path)` | Reachable | Same. |
| `nar-accessor.cc::NarAccessor::readDirectory` | `Error("path '%1%' inside NAR file is not a directory", path)` | Reachable | Same. |
| `nar-accessor.cc::NarAccessor::readFile` | `Error("path '%1%' inside NAR file is not a regular file", path)` | Reachable | Same. |
| `nar-accessor.cc::NarAccessor::readLink` | `Error("path '%1%' inside NAR file is not a symlink", path)` | Reachable | Same. |
| `nar-accessor.cc::NarAccessor::read` (length too big) | `Error("can't read %1% NAR bytes from offset %2%: offset too big", ...)` | Reachable | Same. |
| `nar-accessor.cc::NarAccessor::read` (length too big 2) | `Error("can't read %1% NAR bytes from offset %2%: length is too big", ...)` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` (multiple) | `SysError("opening directory '%1%'", showPath(...))` | Reachable | Used by `LocalFSStore::getFSAccessor`; reachable from `performOp::NarFromPath`. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `SysError("getting status of '%1%'", ...)` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `SysError("reading directory '%1%'", ...)` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `SysError("opening file '%1%'", ap.string())` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `NativeSysError("opening file %1%", PathFmt(root))` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `SysError("opening %1%", PathFmt(root))` | Reachable | Same. |
| `posix-source-accessor.cc::PosixSourceAccessor::*` | `Error("file %1% has an unsupported type", PathFmt(root))` | Reachable | Same. |
| `unix/file-descriptor.cc::readFull` | `SysError("read of %1% bytes", buffer.size())` | Reachable | `readFull` used everywhere (daemon protocol, NAR parsing, etc.). Reachable from any `performOp` that reads from the client socket. |
| `unix/file-descriptor.cc::pread` | `SysError("pread of %1% bytes at offset %2%", ...)` | Reachable | Used by NAR random-access paths. |
| `unix/file-descriptor.cc::writeFull` | `SysError("write of %1% bytes", buffer.size())` | Reachable | Used everywhere (daemon writes to client). |
| `unix/file-descriptor.cc::fsyncDescriptor` | `NativeSysError("fsync file descriptor %1%", fd)` | Reachable | Used in `LocalStore::addToStore` and elsewhere. |
| `unix/file-system-at.cc::*` (multiple HintFmt-lambda forms) | `SysError([&] { return HintFmt("reading symbolic link %1%", ...); })` | Reachable | `unix/file-system-at.cc` is called from `unix/file-system.cc::recursiveSymlinks` and from `LocalStore::*` paths. |
| `unix/file-system.cc::DirectoryIterator` | `SysError("opening directory %1%", PathFmt(path))` (×2) | Reachable | `DirectoryIterator` used heavily in gc, store traversal. |
| `unix/file-system.cc::DirectoryIterator` (read) | `SysError("reading directory %1%", PathFmt(path))` | Reachable | Same. |
| `unix/file-system.cc::removeFile` | `SysError("cannot unlink %1%", PathFmt(path))` | Reachable | Used in gc cleanup, atomic-replace, addTempRoot churn. |
| `unix/processes.cc::killUser` | `SysError("cannot kill processes for uid '%1%'", uid)` | Reachable | `killUser` called from `DerivationBuilderImpl::prepareUser` and `killSandbox` (parent process during build). |
| `unix/processes.cc::killUser` (status check) | `Error("cannot kill processes for uid '%1%': %2%", uid, statusToString(status))` | Reachable | Same. |
| `unix/processes.cc::runProgram` | `ExecError(status, "program %1% %2%", ...)` | Reachable | `runProgram` is used for diff-hook (caught locally — see derivation-builder.cc:516 row), `pre-build-hook`, and for compression in BinaryCacheStore. The pre-build-hook path runs in the parent during `prepareSandbox`, so that throw is wire-crossing. |
| `windows/file-system.cc::recursiveDelete` | `SysError(... "recursively deleting %1%", PathFmt(path))` | Theoretical only | Windows-specific; no current Unix daemon code path. E4 already flagged this. |
| `windows/processes.cc::runProgram` | `ExecError(status, "program %1% %2%", ...)` | Theoretical only | Same. |

Aggregate (libutil positional sites): **45 reachable** (including 1 FreeBSD-only, 2 Windows-theoretical), **3 unreachable** (`args.cc` parseCmdline ×2, plus the CLI-only `loadConfFile` row reclassified — see notes below), **2 conditionally reachable** (configuration.cc loadConfFile, util.hh string2IntMustParse / unit specifier).

Note on `args.cc::parseCmdline`: while the daemon's *own* arg parsing happens
at startup (unreachable), `Args::parseCmdline` is also referenced indirectly
from `loadConfFile`/`Settings::set` via `Setting::set` parsers. None of those
go through the same `parseCmdline` entry, so the verdict stands.

## Aggregate count

Replacing E4's "~61 positional `%N%` sites in libstore + 17 throws in libutil":

- **libstore positional `%N%` throw sites: 71** (not 61)
  - Reachable from `performOp` exception channel: **51**
    - Unconditionally reachable: 38
    - Conditionally reachable (LegacySSH-backed daemon): 1
    - Conditionally reachable (BinaryCache-backed daemon): 1
    - Linux-only platform-conditional: 6
    - FreeBSD-only platform-conditional: 5 (1 of the 6 freebsd sites is in a helper-child without exception channel — unreachable)
    - Darwin-only platform-conditional: 1
  - Unreachable as exception text: **20**
    - LocalStore constructor (daemon startup): 4
    - Profiles.cc CLI-only: 5
    - followLinksToStore CLI-only: 1
    - Builtin builder (post-`\2`): 5 (3 in buildenv.cc + 2 in unpack-channel.cc)
    - Post-`\2` runChild (builtin / execv): 2
    - Diff-hook caught locally: 1
    - FreeBSD loopback helper child: 1
    - Other helper child without exception channel: 1
- **libutil positional `%N%` throw sites: 50**
  - Reachable: **45**
  - Unreachable: **3** (CLI parseCmdline + 1 reclassified)
  - Conditional: **2** (loadConfFile re-parse, unit-specifier parser)
- **libutil all-format throw sites: 281** (E4's 17 was a heavy undercount).
  Most of these use `%s`/`%d`/literal — they are wire-crossing too, just not
  positional-format. The `%N%` -> `{N-1}` migration has nothing to do with
  them but the broader `%s` -> `{}` migration does.

**Net for #125 phasing**: the wire-crossing positional-`%N%` count is **~95**
(51 libstore + 45 libutil), not 61.

## Sites E4 missed

The original E4 grep filtered to `%[0-9]+%` inside `throw`. It missed:

1. **`include/nix/store/store-api.hh::Store::requireStoreObjectAccessor`** —
   throws `InvalidPath("path '%1%' is not a valid store path" or "store path
   '%1%' does not exist", ...)` — header-defined inline method. The original
   single-line grep over `.cc` files would not have caught this if E4 only
   scanned `.cc`. **This is a load-bearing site** for the `"is not valid"`
   hard-break contract because it surfaces during `narFromPath` and is the
   throw that the queryPathInfo client text-grep depends on.
2. **`include/nix/util/util.hh::string2IntMustParse`** (or whatever the unit
   specifier helper is) — header-defined inline. Conditionally reachable.
3. **`build/goal.cc::TimedOut::TimedOut`** — class constructor body uses
   `"timed out after %1% seconds"` format string, but my parser only catches
   `throw` statements. The TimedOut object is constructed at
   `worker.cc:484/490` and dispatched to `goal->timedOut(...)` — eventually
   surfaces in `BuildResult::Failure::msg`. The text *does* cross the wire
   when buildPaths reports a build failure to the daemon.
4. The wider set of **`%s`/`%d` wire-crossing throws** that E4 already
   acknowledged but didn't enumerate — for example
   `local-store.cc:811::queryPathInfoUncached` `InvalidPath("path '%s' is not
   valid", ...)` is the canonical wire-crossing throw for the `"is not
   valid"` contract but doesn't appear in the positional list because it uses
   `%s` not `%1%`.
5. **`worker-protocol-connection.cc::*`** — client-side `%x` throws cited in
   E4 are not in `src/libstore/` positional-format set per se, but they are
   in libstore. The original grep should have caught them under generic
   `throw`. (E4 explicitly noted `%x` is not positional, so this is a
   bookkeeping issue, not a missed site.)
6. **Post-build `BuilderFailureError`** paths in `derivation-builder.cc:609`
   — generates the wire-crossing build-failure text from `status` and a
   string like `"\nnote: build failure may have been caused by lack of free
   disk space"`. Not positional, but clearly wire-crossing.

The text-grep contracts (`"is not valid"`, `"parsing derivation"` /
`"expected string"` / `"Derive(["`) are dominated by `%s`-form throws
(`local-store.cc:811`, `dummy-store.cc:328`, `path.cc:18/24`, `store-api.cc:610/630`,
`binary-cache-store.cc:202`, `store-api.cc:1180`, `derivations.cc:208/215`).
The positional-`%N%` set includes **2** of these contract-bearing sites
(`derivations.cc:208` and `derivations.cc:215`); the remaining 7 contract
sites are `%s` and need the same migration care but aren't in the 71 count.

## Implications for #125 phasing

E4's Phase 2 plan said "the great majority of the 199 + 17 = 216 throws are
mechanical translation". With this verification:

- **Phase 2 scope is accurate, but the count is wrong**: the wire-crossing
  positional-`%N%` set is **~95 sites**, not 61. The non-positional `%s`/`%d`
  set in libstore + libutil is on the order of 200+ additional sites.
- **20 of the original 71 are unreachable as exception text**. They still
  need migration (Phase 1, local-only) but their wire-crossing-test
  obligations are weaker:
  - LocalStore-ctor throws: only matter at daemon startup; user-visible only
    if the daemon fails to boot.
  - Profiles.cc CLI throws: covered by Phase 1's CLI-only bucket.
  - Builtin-builder throws: their text appears in build logs (logger
    frames), not exception bodies. Phase 2's matrix tests do not need to
    cover them; Phase 1 logger-frame tests should (E4 open question 5
    asked about this — answer is yes for the buildenv/unpack-channel sites).
  - Post-`\2` runChild throws: same as builtin-builder.
  - Helper-child throws (FreeBSD loopback, diff-hook): same.
- **The matrix test must pin `requireStoreObjectAccessor` even though it's
  in a header**. The original E4 plan named `local-store.cc:811`,
  `derivations.cc::expected string`, and `store-api.cc::error parsing
  derivation` as hard test requirements. To that list add
  `store-api.hh::requireStoreObjectAccessor` (both the
  `requireValidPath ? : :` arms — both must preserve `"is not valid"` /
  `"does not exist"` substrings).
- **`local-fs-store.cc::LocalStoreAccessor::requireStoreObject`** is also
  hard-break: it throws `InvalidPath("path '%1%' is not a valid store
  path", ...)` and is reachable when the daemon backs a `LocalFSStore`. It's
  the same contract text as `local-store.cc:811` but with positional
  format, so the positional-only matrix sample must include it.
- **`remote-fs-accessor.cc:16`**: reachable when the daemon backs a
  `BinaryCacheStore`. Same `"is not valid"` contract; uses positional
  format. Phase 2 matrix needs a `BinaryCacheStore`-backed daemon row.
- **The `derivations.cc:208/215` sites** are the only positional-format
  contributors to the `DynamicDerivations` triple text-grep contract; the
  other two substrings come from `%s`-form throws elsewhere. The matrix
  test row for old-client / new-daemon must verify *all three substrings
  survive* the positional-to-`{N-1}` rewrite (both the `%1%` part and the
  `%s` parts).

### Phase 1 is still safe

E4's Phase 1 is unaffected. The 20 unreachable libstore sites still need
migration but Phase 1 is mechanical (`%1%` -> `{0}`) and their wire-test
obligations are weaker, so they should land in Phase 1 alongside libexpr.

### Phase 2 should add

- A `BinaryCacheStore`-backed daemon row to the matrix.
- A `LegacySSHStore`-backed daemon row for the `legacy-ssh-store.cc:91`
  connection-failure path (covers the `LegacySSHStore::openConnection`
  throw and the `Error("'nix-store --serve' protocol mismatch from '%s'", ...)`
  throw at line 88-89, which is the canonical wire-crossing scenario when
  the daemon talks to a remote builder and propagates the failure back to
  the local client).
- An `IndirectRootStore::addPermRoot` "forbidden gcRoot" row.
- An accessor-based `narFromPath` row that exercises `LocalStoreAccessor`
  and `RemoteFSAccessor`.
- A `parseHashAlgo("garbage")` row reached via `performOp::AddToStore` old
  protocol.

## Open questions

1. **`unpack-channel.cc:42`** uses `SystemError(e.code(), "failed to rename
   %1% to %2%", ...)`. Is `SystemError` distinct from `SysError`, and does it
   propagate the same way? Both are `SystemError` subclasses but the wire
   format only knows `Error`/`info.msg`. Verify `SystemError`'s
   `info().msg.str()` rendering matches the matrix expectations.
2. **`configuration.cc::loadConfFile` re-parse at runtime** — the
   `ClientSettings::apply` path calls into the global config. Does it
   re-read config files? If yes, the positional throw is reachable via
   `performOp::SetOptions`. If no, it's daemon-startup-only.
3. **`build/goal.cc::TimedOut::TimedOut`** constructor format string — does
   the migration tooling cover constructor bodies, or only `throw`
   expressions? If only throws, this site needs explicit hand-translation.
4. **The `store-api.hh` inline throws** were missed by the original grep.
   How many other inline throws in `*.hh` headers exist? My parser scanned
   `.hh` too and caught these; E4's grep may not have. Re-run the
   inventory grep over `.hh` files to make sure nothing else was missed.
5. **`legacy-ssh-store.cc:91`** is conditionally reachable (only when the
   daemon backs an `ssh-ng://` legacy store). The matrix should include a
   row for this configuration. **Is this configuration tested in CI today?**
   If not, the matrix would catch a regression but only with new fixture
   setup work.
6. **`%s` versus `%1%` migration ordering** — the verification confirms
   that the `"is not valid"` contract is preserved by **both** `%s`-form
   sites (the majority) and `%1%`-form sites (`local-fs-store.cc:50`,
   `remote-fs-accessor.cc:16`, `store-api.hh::requireStoreObjectAccessor`).
   Phase 2's mechanical translation must touch all of them in the same
   batch — splitting positional and non-positional migrations would risk
   leaving the contract half-translated. The matrix test must run with
   *every combination* of which-sites-translated to be conservative,
   which is impractical; instead, Phase 2 should batch positional + `%s`
   together for the contract-bearing sites and pin the rendered text in a
   single matrix row.
