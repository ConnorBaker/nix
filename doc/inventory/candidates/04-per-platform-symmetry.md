# Per-platform symmetry

Candidates 26-29. All four VALID. The Windows symmetry candidates (#26-27,
#29) are real but ship effectively untested — `windows_tests` in
`.github/workflows/ci.yml` is `continue-on-error: true` and only builds
`nix-util-tests`. Cross-reference [#161](17-cross-cutting.md).

| # | Verdict | Effort |
| - | ------- | ------ |
| 26 | VALID | small |
| 27 | VALID | structural |
| 28 | VALID | structural |
| 29 | VALID | trivial |

---

26. **`pathlocks` Unix vs Windows.** `lockPaths` (sorted iteration, retry on stale lock detected by `<file>.lock` not being empty) and `FdLock`'s constructor (try non-blocking, then blocking with `printInfo(waitMsg)`) are duplicated almost verbatim between `unix/pathlocks.cc` and `windows/pathlocks.cc`. Differences are confined to `flock` vs `LockFileEx` and the test for "lock file became stale" (`fstat().st_size` vs `getFileSize`). Could share a generic skeleton with platform shims for `openLockFile`/`lockFile`/`isStale`.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. The Windows file is ~167 LoC including a Wine workaround. Refactor ships untested on Windows under current CI scope. Effort: small (mechanical) — but the shared skeleton must mechanically reproduce the existing `LockFileEx` two-region trick (write at offset 1, read at offset 0).
    - **Branch:** `vibe-coding/cleanup/libstore` (hoisted `PathLocks::lockPaths` and `FdLock::FdLock` from the per-platform files into the shared `pathlocks.cc`; staleness check uses portable `getFileSize`. The Windows two-region `LockFileEx` trick stays inside the platform `lockFile` shim, unchanged. Linux build green; Windows ships untested under current CI scope.).

27. **File-system / file-descriptor / processes per-platform implementations parallel each other.** `unix/file-system.cc` vs `windows/file-system.cc`; `unix/file-descriptor.cc` vs `windows/file-descriptor.cc`; `unix/processes.cc` vs `windows/processes.cc`; `unix/environment-variables.cc` vs `windows/environment-variables.cc`; `unix/users.cc` vs `windows/users.cc`. Each pair declares the same logical API but with different signatures (Windows takes wide strings as `OsString`; `Pipe::create` has no `nonBlocking` parameter on Windows; `MuxablePipePollState::poll` takes an extra `HANDLE ioport` on Windows; `Pid` holds a `pid_t` on Unix and an `AutoCloseFD` on Windows).
    - ../verified/04-libutil-misc.md
    - **Validation:** VALID. Not a mechanical edit but a strategic shift: Group A (signatures already match — `getCpuUserTime`, `read`/`write`/`getFileSize`, `lstat`/`maybeLstat`, `runProgram`) needs no change; Group B (signatures diverge — `Pipe::create` arity, narrow/wide env-var pairs, `Pid` carrying `pid_t` vs `AutoCloseFD`, `MuxablePipePollState::poll` taking an extra `HANDLE`) wants a unified `PipeOptions` struct, `OsString`-based env API, and an interface-style `Pid`; Group C (genuinely platform-only — cgroups, jails, signal handler thread, IOCP) has no mirror. Effort: structural. Compounds with #163, #164.

28. **Goal builders by platform form a diamond hierarchy.** `DerivationBuilderImpl` (in `libstore/unix/build/derivation-builder.cc`) is the unix base; `ChrootDerivationBuilder` (`unix/build/chroot-derivation-builder.cc`, `virtual` inheriting `DerivationBuilderImpl`) is the Linux/FreeBSD shared chroot fragment; `LinuxDerivationBuilder`/`FreeBSDDerivationBuilder` (in `unix/build/linux-derivation-builder.cc` and `unix/build/freebsd-derivation-builder.cc`, both `virtual` inheriting `DerivationBuilderImpl`) are the platform-specific siblings; `ChrootLinuxDerivationBuilder`/`ChrootFreeBSDDerivationBuilder` are the diamond merges (multiply inheriting `ChrootDerivationBuilder` and the platform builder); `DarwinDerivationBuilder` (`unix/build/darwin-derivation-builder.cc`) and `ExternalDerivationBuilder` (`unix/build/external-derivation-builder.cc`) sit beside as siblings of the diamond, both directly inheriting `DerivationBuilderImpl`. The five platform/chroot files are pulled together via `#include` directives at the bottom of `unix/build/derivation-builder.cc`. Many overrides (`setBuildTmpDir`, `tmpDirInSandbox`, `prepareSandbox`, `enterChroot`, `setUser`, `startChild`, `cleanupBuild`, `killSandbox`, `getBuildUser`, `addDependencyImpl`, `needsHashRewrite`) follow a pattern of "base then platform-specific suffix" that could be expressed as separable strategy hooks.
    - ../verified/10-libstore-build.md
    - **Validation:** VALID. The diamond expresses "platform sandbox × chroot-or-not", which is a Cartesian product, not a hierarchy. Strategy/policy split is the correct modernisation target. The `#include` at the bottom is a unity-style build artefact. Pitfall: sandbox semantics are subtle (PID/user namespaces, jails, `prctl(PR_SET_PDEATHSIG)`), so the diamond is doing real work and a one-shot rewrite is unwise — phase via (1) extract pure helpers from each override, (2) introduce strategy types alongside the existing classes, (3) delete the diamond once tests catch up. Effort: structural. Compounds with #34, #155.

29. **XDG dirs (Unix) vs known folders (Windows).** `nix::unix::xdg::getCacheHome`/`getConfigHome`/`getConfigDirs`/`getDataHome`/`getStateHome` parallel `nix::windows::known_folders::getLocalAppData`/`getRoamingAppData`/`getProgramData`. The wrapping `getCacheDir`/`getConfigDir`/... in `users.cc` switches on `_WIN32` per call. A platform-conditional table would be cleaner.
    - ../verified/03-libutil-runtime.md, ../verified/04-libutil-misc.md
    - **Validation:** VALID. Replace per-call `#ifdef _WIN32` with a five-row × two-platform table populated once at startup; on Windows the five logical directories collapse onto `LocalAppData`/`RoamingAppData`/`ProgramData`. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libutil` (cached the platform defaults once at first access; `NIX_*_HOME` env reads remain per-call, but the underlying `XDG_*_HOME`/`XDG_CONFIG_DIRS` reads are now cached — benign for in-tree callers, may affect embedders that re-export XDG vars mid-process)
