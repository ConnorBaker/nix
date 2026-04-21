# `--build-debugger` — handoff plan

A prioritized, self-contained handoff for the remaining work on Nix's
`--build-debugger` feature. Written for a fresh agent picking up mid-stream.

---

## Status snapshot (updated 2026-04-20, post-adversarial-review)

**Work items 02, 03, 04, 05, 06, 07, 08, 10, 14, 16, 18 are
implemented.** See the strike-throughs in §Work below.

**Adversarial review fixes applied 2026-04-20 (three rounds)** (see §Review fixes
below for details):

- `exit-code-passthrough.nix` was racing: it signaled the wrapper on
  attach-info presence rather than env-vars presence, killing bash
  with 143 before the wrapper installed its TERM trap. Test now gates
  on `test -f "$tmp/env-vars"` like every other split file.
- Wrapper's `_nix_debug_attach` now installs `trap ':' TERM HUP INT`
  at the top of the function rather than immediately before `wait`,
  closing a race window where a SIGTERM arriving during env-vars
  capture would kill bash with 143.
- `shouldApplyBuildDebugger()` result is now cached per-builder; was
  called ~5 times per build with full `weakly_canonical` each time.
- `closeExtraFDs(preserve)` signature widening reverted — no
  production caller used the parameter; it was staged for a design
  that didn't ship. Unit tests covering the preserve branches removed.
- `debug-attach.cc`'s initial liveness check switched from
  `kill(pid, 0)` to `pidfdAlive(pidfd)` (per followups `What NOT to
  do` §kill(pid,0)). The pidfd is now opened before the check so its
  result feeds both the "paused yet?" and final-pre-fork liveness
  probes.
- `publishDebuggerAttachInfo`'s inode-mismatch assertion is now a
  warning. It was called from `startBuild` after `startChild()`, so
  throwing there leaked the already-forked child.
- External-builder refusal in `buildLocally` deleted — the
  `ExternalDerivationBuilder` ctor's refusal is authoritative; the
  goal-side check was a duplicate with identical error text.
- Serve-protocol version warning in `legacy-ssh-store.cc` now dedups
  per-host (was a global `std::atomic<bool> warned` that masked
  warnings for legacy peers hit after a new-enough peer).

Deferred / rescinded:

  - **Work-item 01** (`nix debugger-gc` subcommand) — *rescinded*. The
    command existed only so tests could deterministically trigger the
    sweep without waiting 60 s. In production the daemon-side sweep
    already self-cleans the directory on the next `--build-debugger`
    run; a manual CLI adds surface area for no recurring need. Removed
    along with `CmdDebugList` and the ancillary drvPath-sanitisation /
    hash-column code (work-item 09's output was just cosmetic polish
    on a command that no longer exists).
  - **Work-item 09** (debug-list sanitisation + hash column) —
    *rescinded*. `nix debug-list` itself has been removed; the
    command was discoverable only through 'what builds are paused?'
    curiosity, answerable with `ls /nix/var/nix/debugger/*.attach`.
    Display sanitisation is no longer needed.
  - **Work-item 11** (--timeout / --wait live-build test) —
    *rescinded*. The `--wait` flag it tested has also been removed.
    The attach CLI now fails fast with guidance instead of polling —
    the only legitimate "user ran debug-attach before the failure
    printed" scenario was contrived, and hanging 600 s as the default
    was never worth the complexity.
  - **Work-item 12** (force-success escape-hatch test) — *rescinded*.
    The mechanism documented in `opt-common.md` (user runs
    `trap - EXIT; exit 0` inside the debug shell to flip the build to
    successful) does not actually work: the wrapper's
    `_nix_debug_attach` always ends with `return "$ec"`, which becomes
    the bash process's exit status regardless of what the detached
    attach-shell did. `opt-common.md` has been corrected.
  - **Work-item 13** (debug-list mixed-state test) — *rescinded* along
    with the command.
  - **Work-item 15** (serve-protocol version-warning test) — requires a
    pinned older Nix with serve protocol <2.9, and the plan-specified
    candidate may not still ship.
  - **Work-item 17** (concurrent-sweep race test) — **implemented**.
    `tests/nixos/build-debugger/security-sweep.nix` uses a `nix-daemon`
    restart to reset the sweep's time guard between two builds rather
    than parallelising. That doesn't exercise literal concurrency, but
    it does exercise sweep correctness with a mix of fresh + stale +
    aged + redirect-only entries; the race the original concern named
    was about sweep *correctness* under concurrent publishes, not
    specifically about wall-clock parallelism.

**Previous `security` subtest 19 flakiness, root-caused and fixed in the
finer-split layout:** the forged-attach-info subtest used to create
`/build` on the host (to satisfy `sandboxTmpdir`/`hostTmpdir` fields in
the forgery). Without cleanup, the next real build's
`publishDebuggerAttachInfo` inode-equality check stat'd the host's
`/build` against the sandbox's bind-mount source and — since the
manually-created `/build` was a different inode — correctly refused to
publish. In the 14-file split, each subtest runs in its own VM, so
state can't leak between them.

**Test layout** (10 test files + shared `common.nix` under
`tests/nixos/build-debugger/`, each registered as a top-level
`build-debugger-<name>` hydra job via `lib.mapAttrs'` in
`tests/nixos/default.nix`):

- `common.nix` — shared node config (experimental features,
  trusted-users, writableStore, shared additionalPaths).
- `gates.nix` — static CLI-parse-time refusals (no builds).
- `attach-{happy,scoping,success-noop}.nix` — the three attach-path
  invariants, one VM each.
- `bare-bash.nix` / `structured-attrs.nix` — non-stdenv variants.
- `exit-code-passthrough.nix` — `exit N` in buildPhase propagates.
- `security-{proc-check,flock}.nix` — /proc cmdline check + flock.
- `remote.nix` — two-node remote-builder end-to-end.

All 10 run in ~1.5 min wall-clock when built locally (`nix build
... --builders ''`); the wall-clock bottleneck is the `remote` file
(two VMs) and `security-flock` (one real build with a flock helper).
Each other test finishes in 10–17 s.

Each file does at most one `--build-debugger` build, so wall time per
file is bounded by one build-phase cost (~30–90 s inside the VM plus
~12 s boot). On a multi-core builder the suite parallelises to wall
time ≈ max-across-files ≈ 3 min (dominated by `remote`, which runs
two VMs).

**Branch**: `vibe-coding/busybox-breakpoint` (check before starting).

**Feature state**: pivot from daemon-PTY-handoff to nsenter-based design is
complete. Security hardening, remote-builder propagation via serve protocol
2.9, and `nix debug-attach`/`nix debug-list` subcommands exist. Remote-
builder path now verified end-to-end via `tests/nixos/build-debugger-remote.nix`
(two-node topology, redirect-on-dispatch, auto-SSH follow, explicit `--on`).

**Test state**:
- `nix build .#nix-everything`: passes.
- The monolithic `tests/nixos/build-debugger.nix` has been deleted; its
  subtests are redistributed across seven files (see §Work item 02).
- Split tests: gates, attach, bare-bash, structured-attrs, exit-code,
  remote all pass. security fails at subtest 19 as noted above.

**Out of scope** (confirmed with user, do not attempt):
- Content-addressed / fixed-output derivations.
- External builders (`external-builders` experimental feature).

---

## Pre-flight (do this before any work)

```bash
# 1. Baseline the tree.
git status --short                            # expect: clean modulo this plan
git log --oneline -5                          # confirm you're on the right HEAD

# 2. Confirm the current build is green.
nix build .#nix-everything --no-link -j0      # should exit 0

# 3. Confirm the known test state.
nix build .#hydraJobs.tests.build-debugger -L --no-link -j0
# Expect exit 1 with subtest 17's "stale-attach-info sweep" traceback.
# Every OTHER subtest must pass. If any other fails, stop and figure out
# why BEFORE starting work items — something has regressed from the
# handoff baseline.
```

If any of the above fail in unexpected ways, **do not start work items**.
Investigate the regression first; the plan below assumes the baseline
above.

---

## Definition of Done (the whole feature)

Build-debugger v1 ships when **all** of the following hold:

1. All work items in §Work below are closed (done, struck with rationale,
   or deferred with a concrete rationale linked back to this file).
2. `nix build .#nix-everything --no-link -j0` exits 0.
3. `nix build .#hydraJobs.tests.build-debugger --no-link -j0` exits 0.
4. `nix build .#hydraJobs.tests.build-debugger-remote --no-link -j0` exits
   0 (new test, see `work-item 02`).
5. `doc/manual/source/command-ref/opt-common.md` and
   `src/nix/debug-attach.md` match actual behavior.
6. A release-notes entry exists in `doc/manual/source/release-notes/`.
7. No `// TODO: see research/build-debugger-followups.md §X` comments in
   code — every such comment should be resolved (the item done, or
   renumbered/re-linked if the plan changes).

---

## Gotchas — things you WILL re-encounter

These are verbatim pitfalls from the previous iteration. Reading them
saves ~1 hour per pitfall.

### VM test harness (`tests/nixos/build-debugger.nix`)

- **`log` is a reserved name** in the NixOS test driver (it's an
  `AbstractLogger` instance in the exec scope). Using `log = machine.succeed(...)`
  trips a type-check error. Rename to `build_log`, `out_log`, etc.
- **f-strings must contain a `{…}` placeholder.** Pyflakes rejects
  `f"literal text"`. Drop the `f` prefix in those cases.
- **`--timeout` is a global Nix build-timeout setting and will shadow
  any subcommand flag of the same name.** If you add a new flag to
  `CmdDebugAttach` that takes a duration, don't call it `--timeout`;
  it'll silently lose to the setting parser and the flag's handler
  won't run. Pick a different name (we spent an afternoon debugging
  this before realising the collision).
- **Nested single quotes break `bash -c '…'`.** The `NIX` constant in the
  test is `"nix --extra-experimental-features 'nix-command …'"` —
  embedding that inside another single-quoted string corrupts quoting.
  Use `cat > script.sh <<'EOF' … EOF; chmod +x script.sh; systemd-run …`
  pattern instead.
- **Background processes must survive the test-driver command**. The test
  driver's commands get reaped when they return. Use
  `systemd-run --quiet --unit=bd-<name> --no-block --property=StandardInput=null /path/to/script.sh`.
  `setsid nohup &` alone does not reliably detach.
- **The test VM must have `python3` in `environment.systemPackages`** if
  `machine.execute(...)` ever pipes through Python — the test driver's
  spawned shells use the VM's PATH, not the host's.
- **VM-test builds take ~10 minutes each**. Factor in iteration time when
  choosing whether to add a subtest vs. create a new VM-test file.

### Nix-language build fixtures

- **Bare-bash derivations need `coreutils` wired in explicitly.** The
  wrapper uses `sleep` (and a few other builtins). For test fixtures use
  `coreutils = builtins.storePath "${pkgs.coreutils}";` and export
  `PATH=${coreutils}/bin:$PATH` before `exit`.
- **`builtins.storePath` requires `--impure`.** When instantiating test
  drvs from `nix-instantiate`, pass `--impure`. (The existing test uses
  `nix-instantiate --extra-experimental-features …` without `--impure`
  because the drvs use `with import <nixpkgs> { }` which is already
  impure.)
- **`gnugrep` is not in `coreutils`.** Any wrapper bash that needs `grep`
  must add gnugrep separately. (The current wrapper is pure-bash via
  `[[ =~ ]]` regex — keep it that way.)

### C++ / Linux APIs

- **`kill(pid, 0)` is race-prone.** A reused pid can succeed the check
  while being an unrelated process. Use `pidfd_send_signal(pidfd, 0, …)`
  once the pidfd refactor lands (`work-item 05`).
- **`nsenter --pidfd` does not exist.** util-linux's `nsenter` takes
  `--target <pid>` only. Race-free entry requires in-process `setns(pidfd, …)`.
- **`setns(2)` is order-sensitive**: enter the user namespace FIRST when
  remapping uid/gid, else `setuid()` later will fail with EPERM.
- **`pidfd_open(2)` needs Linux ≥ 5.3.** `setns(pidfd, flags)` needs Linux
  ≥ 5.8. Per user policy, both requirements are acceptable; surface a
  clear error on older kernels instead of silent fallback.
- **The wrapper's sandbox-side PATH is whatever the derivation sets.**
  Do not assume any binary is in PATH at trap-fire time; rely only on
  bash builtins, the path embedded in `_nix_debug_bash`, and what the
  derivation has explicitly set.

### Conventions

- **No trailing periods in error messages** (Nix style).
- **Use `HintFmt` for interpolated error messages.**
- **`printError`** lands in the build log, easy to miss. Prefer
  `logWarning({.msg = HintFmt(…)})` for user-relevant warnings.
- **`printInfo`** for progress-style user feedback; gets the verbosity-level
  treatment users expect.

---

## Work items (priority order)

Each item has: **Problem**, **Approach**, **Files** (by function name, not
line number — line numbers drift), **Depends on**, **Done when**.

### 01. `nix debugger-gc` subcommand (fixes subtest 17)

**Problem**: `maybeSweepStaleDebuggerAttachInfo` has a 60 s time guard, so
a VM test that forges a stale entry then triggers a real `--build-debugger`
build can't rely on the sweep firing. Subtest 17 of the main VM test is
blocked on this.

**Approach**: add a `CmdDebuggerGc` subcommand alongside the existing
`CmdDebugList` in `src/nix/debug-attach.cc`. It scans the debugger dir and
removes stale `.attach` files unconditionally (no time guard). Same
staleness definition as the sweep: missing/dead pid AND no `remoteHost`,
or — once `work-item 08` lands — age > 6 h. Use it in subtest 17 to
deterministically trigger cleanup.

Also useful to users for "my daemon crashed mid-build, clean up the
orphans".

**Files**:
- `src/nix/debug-attach.cc` — add `CmdDebuggerGc` and
  `registerCommand<CmdDebuggerGc>("debugger-gc");` near `CmdDebugList`.
- `src/nix/debug-attach.md` — brief mention of the new subcommand (or
  consider `debugger-gc.md` if a full doc page is warranted).
- `tests/nixos/build-debugger.nix` — rewrite subtest 17 to call
  `nix debugger-gc` rather than rely on the sweep firing.

**Depends on**: none.

**Done when**:
- Running `nix debugger-gc` with no arguments removes stale `.attach`
  files and reports counts (e.g. "removed 3 stale entries").
- Subtest 17 passes.
- `nix build .#hydraJobs.tests.build-debugger` exits 0.

---

### 02. Split the monolithic VM test into parallel files

**Problem**: `tests/nixos/build-debugger.nix` is a single VM test with 21
subtests running sequentially. Each subtest boots one QEMU; total
runtime is ~10 min. The NixOS test framework runs each VM test in its
own sandbox, so **separate test files run in parallel on a multi-core
builder** — but subtests inside one file do not. The longer the file
grows, the worse the serialization, and the more likely a flaky subtest
wedges the whole suite.

Also: every subsequent work item below ("add a subtest for X") piles
onto the same file, compounding the problem.

**Approach**: split the monolith into themed files that each focus on one
coherent set of invariants, sharing no per-test mutable state. Each file
should build + attach at most a small number of derivations so its VM
runtime stays under ~3 min.

**Proposed split** (adjust names if the codebase has a naming convention
for multi-file NixOS tests):

- **`build-debugger-gates.nix`** — static refusals and non-build
  subtests. No actual builds. Should run in well under a minute.
  - Experimental-feature gate, `nix develop --build-debugger` rejection,
    non-bash builder refusal, inline-`-c` refusal, CA refusal,
    untrusted-users refusal, `nix debug-attach` root-check,
    `--timeout` against nonexistent drv, malformed-drv-path rejection,
    schema-version rejection, `--on HOST` SSH-unreachable mapping,
    `nix debug-list` empty listing.

- **`build-debugger-attach.nix`** — the core happy-path: one stdenv
  failing build, attach via `nix debug-attach`, inspect env, exit.
  Covers the sandbox preservation, env capture, scoping invariant,
  and the `0700` dir permission assertion.

- **`build-debugger-bare-bash.nix`** — non-stdenv EXIT-trap path with
  a bare-bash derivation. Verifies `declare -p` env capture works
  outside stdenv and the wrapper's pure-bash fallback is correct.

- **`build-debugger-structured-attrs.nix`** — `__structuredAttrs = true`
  derivation, verifies env capture picks up shell-only (non-exported)
  variables via `declare -p`.

- **`build-debugger-exit-code.nix`** — explicit `exit N` passthrough,
  plain `exit` preserving original exit code, "force success"
  (`trap - EXIT; exit 0`) — see `work-item 12`.

- **`build-debugger-security.nix`** — `/proc/<pid>/cmdline` verification,
  concurrent `flock` serialization, stale-attach-info sweep (uses
  `nix debugger-gc` from `work-item 01`).

- **`build-debugger-remote.nix`** (new, also the deliverable for the
  two-node remote end-to-end test originally numbered 02) — two-node
  topology with `nix.buildMachines` dispatch.

Each file registers in `tests/nixos/default.nix` alongside the existing
`build-debugger = runNixOSTest ./build-debugger.nix;` entry. Use a
naming pattern like:

```nix
build-debugger-gates          = runNixOSTest ./build-debugger-gates.nix;
build-debugger-attach         = runNixOSTest ./build-debugger-attach.nix;
build-debugger-bare-bash      = runNixOSTest ./build-debugger-bare-bash.nix;
build-debugger-structured-attrs = runNixOSTest ./build-debugger-structured-attrs.nix;
build-debugger-exit-code      = runNixOSTest ./build-debugger-exit-code.nix;
build-debugger-security       = runNixOSTest ./build-debugger-security.nix;
build-debugger-remote         = runNixOSTest ./build-debugger-remote.nix;
```

**Shared infrastructure**: factor common node configuration into a
shared Nix module (e.g. `tests/nixos/build-debugger-common.nix`) and
`imports` it from each test file. Cover:
- Experimental features list.
- `trusted-users` / `allowed-users`.
- `python3` in `systemPackages`.
- `virtualisation.writableStore = true` and the shared `additionalPaths`
  (`pkgs.bash`, `pkgs.coreutils`, etc.).
- Common `/etc/test-drvs/…` fixtures that more than one test needs.

Per-test `.nix` files then only add their specific fixtures and subtests.

**Migration order**:
1. Create `build-debugger-common.nix` with shared config.
2. Create each new test file, move the relevant subtests out of
   `build-debugger.nix`.
3. Delete the emptied subtests from the monolith; keep the monolith
   only until all subtests have homes, then delete the file.
4. Update `tests/nixos/default.nix` registrations.

**Depends on**: `work-item 01` (for the `security` file's sweep subtest
to have `nix debugger-gc` to call).

**Done when**:
- `ls tests/nixos/build-debugger*.nix` shows ≥ 6 files.
- `tests/nixos/build-debugger.nix` no longer exists (or is a minimal
  aggregator that's been deleted).
- Each of the registered tests can be built independently:
  `nix build .#hydraJobs.tests.build-debugger-gates --no-link -j0`, etc.
- Running them all via `nix build .#hydraJobs.tests.build-debugger-{gates,attach,bare-bash,structured-attrs,exit-code,security,remote} --no-link -j0`
  completes substantially faster than the old serial monolith on a
  multi-core builder.
- No subtest is silently dropped during the migration (audit the
  original file's subtests against the sum of the new files).

---

### 03. Two-node VM test: real remote-builder end-to-end

**Problem**: the serve-protocol 2.9 extension, hook-side
redirect-attach-info writer, and `nix debug-attach --on HOST` SSH dispatch
have only been compile-tested. No end-to-end verification that a client's
`nix build --build-debugger` with `nix.buildMachines` dispatches to a
builder, the builder instruments the build, and the user can attach.

**Approach**: land as `tests/nixos/build-debugger-remote.nix` (the
"remote" slot from `work-item 02`). Two-node topology modeled on
`tests/nixos/remote-builds.nix`. Structure:

- `nodes.client`: `nix.buildMachines = [ { hostName = "builder"; sshUser = "root"; sshKey = "/root/.ssh/id_ed25519"; system = "i686-linux"; maxJobs = 1; } ]`,
  `nix.settings.max-jobs = 0` to force remote dispatch. The build target
  must have `system = "i686-linux"` (or another system the client can't
  build locally) so the hook HAS to dispatch.
- `nodes.builder`: `services.openssh.enable = true`,
  `virtualisation.writableStore = true`. Same Nix version as client
  (`nixComponents.nix-cli`).
- Test script sets up SSH keys between client and builder, then runs
  the subtests.

**Subtests**:

1. **Hook dispatches + local redirect written**
   Run `nix build --build-debugger <failing-i686-drv>` on client.
   Verify `/nix/var/nix/debugger/<hash>.attach` on client has
   `"remoteHost": "ssh://root@builder"` (or whatever `hook->machineName`
   produces).

2. **Local `nix debug-attach` auto-SSHes**
   On client, `sudo nix debug-attach <drv>` (no `--on`). It should follow
   the redirect, SSH to builder, and start the attach session on builder.
   Drive canned commands via the `script -qc` + piped-stdin pattern from
   the main VM test. Verify the attach shell sees the build-phase
   sentinel (e.g. `MY_PHASE_SENTINEL`).

3. **Build reports failure on client after attach ends**
   The original `nix build` on client should exit non-zero with the
   original builder's exit code.

4. **Cleanup**
   After the session: client's redirect `.attach` is removed (via
   `~DerivationBuildingGoal`); builder's real attach-info is removed
   (via `unpublishDebuggerAttachInfo` on the remote's cleanupBuild).

5. **Explicit `--on builder`** (bypass redirect)
   With the redirect deleted, run `nix debug-attach --on builder <drv>`.
   It should SSH using builder's config (key/port from
   `nix.buildMachines`, once `work-item 04` lands).

6. **Hook-accept-then-remote-failure cleanup**
   Force the remote to refuse the build (e.g. make its store unwritable
   transiently). The hook accepts but shipping fails. Client's redirect
   `.attach` must be removed — destructor handles this.

**Files**:
- `tests/nixos/build-debugger-remote.nix` (the "remote" slot from
  `work-item 02`).
- `tests/nixos/default.nix` — register the new test (done as part of
  `work-item 02`; this item just populates the file's contents).

**Depends on**: `work-item 02` (test file exists) and `work-item 04`
(otherwise `--on builder` uses wrong SSH config in subtest 5).

**Done when**:
- `nix build .#hydraJobs.tests.build-debugger-remote --no-link -j0`
  exits 0.
- Subtests 1–6 above all assert their expected invariants.

---

### 04. Remote dispatch reuses `nix.buildMachines` SSH configuration

**Problem**: `CmdDebugAttach::dispatchRemote` hardcodes
`ssh -t HOST -- sudo nix debug-attach DRV`. This ignores the `sshUser`,
`sshKey`, `sshPublicHostKey`, port, and `extraSshArgs` fields that the
user configured for the remote builder, forces `sudo` even when
`sshUser == "root"`, and relies on the invoking user's interactive
`ssh` config rather than the daemon's.

**User policy**: match how the build hook authenticates — same key, user,
port as `nix.buildMachines`.

**Approach**:

1. **Lookup.** Parse `nix.buildMachines` (from
   `settings.getWorkerSettings().builders`) via `Machine::parseConfig`.
   Use the existing parser — `src/libstore/machines.cc` already has the
   logic; `src/nix/build-remote/build-remote.cc` uses it as a reference.

2. **Identify the target.** Input is either the `--on HOST` argument or
   the `remoteHost` field from a redirect attach-info. For the redirect
   case, the value is the storeUri (e.g. `ssh://builder1`); strip the
   scheme to get the bare hostname.

3. **Construct the SSH command.** Honor all machine fields:
   ```cpp
   std::vector<std::string> cmd = {"ssh", "-t"};
   if (!machine.sshKey.empty()) {
       cmd.push_back("-i");
       cmd.push_back(machine.sshKey);
   }
   // port, extraSshArgs, …
   cmd.push_back(
       machine.sshUser.empty()
           ? machine.hostName
           : machine.sshUser + "@" + machine.hostName);
   cmd.push_back("--");
   if (machine.sshUser != "root")
       cmd.push_back("sudo");
   cmd.push_back("nix");
   cmd.push_back("debug-attach");
   cmd.push_back(drvPath);
   ```
4. **Fallback.** If `HOST` isn't in `nix.buildMachines`, fall back to the
   current bare-ssh dispatch. Emit a `debug`-level log line so users
   troubleshooting auth failures can see which config path ran.

5. **Access to the machine config.** `nix debug-attach` runs as root (via
   `sudo`) and reads `settings.getWorkerSettings().builders` — same path
   the daemon uses. Should Just Work; verify in VM test
   (`work-item 03` subtest 5).

**Files**:
- `src/nix/debug-attach.cc::CmdDebugAttach::dispatchRemote` — rewrite.
- `src/nix/debug-attach.cc` — add `#include "nix/store/machines.hh"`.

**Depends on**: none (but unblocks `work-item 03` subtest 5).

**Done when**:
- `work-item 03` subtest 5 passes.
- Running `nix debug-attach --on <unknown-host> …` on an unreachable host
  still surfaces the "SSH connection to … failed" error from the existing
  exit-code mapping (the fallback path doesn't break existing coverage).
- Non-root `sshUser` verifies `sudo` is prepended.
- Root `sshUser` verifies `sudo` is NOT prepended.

---

### 05. Race-free `nsenter` via in-process `setns(pidfd, …)`

**Problem**: `dispatchAttach` uses `execvp("nsenter", "--target", <pid>, …)`.
Even with the preceding `pidfd_send_signal(pidfd, 0)` liveness check, a
microsecond window exists where the pid could be reused between the
check and nsenter's own `openat("/proc/<pid>/ns/…")`. `nsenter` takes a
pid number, not a pidfd — there's no way to forward the pin.

Drop `nsenter` entirely; do the namespace entry in-process, keeping the
pidfd alive across the operation.

**Approach**:

1. **Open pidfd** via `syscall(SYS_pidfd_open, pid, 0)`. On `ENOSYS`,
   error with "requires Linux ≥ 5.3 for race-free pid tracking". On
   `ESRCH`, error with "wrapper died before we could attach; re-run the
   build to try again". Any other errno is a genuine SysError.

2. **Verify identity** via `/proc/<pid>/cmdline` check (existing
   `cmdlineContains(pid, ".nix-debug-wrapper.sh")`). Keep this as a
   defence-in-depth check even with pidfd.

3. **Enter namespaces.** Prefer single-call
   `syscall(SYS_setns, pidfd, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNET)`
   on Linux ≥ 5.8. On `ENOSYS` / earlier kernels, fall through to
   per-namespace:
   ```cpp
   for (auto ns : {"user", "mnt", "ipc", "uts", "pid", "net"}) {
       int fd = open(("/proc/" + std::to_string(pid) + "/ns/" + ns).c_str(),
                     O_RDONLY | O_CLOEXEC);
       if (fd < 0) throw SysError("open /proc/%d/ns/%s", pid, ns);
       if (setns(fd, 0) < 0) throw SysError("setns %s", ns);
       close(fd);
   }
   ```
   Order matters: user namespace FIRST (else later uid/gid changes EPERM).

4. **`fork()`** — `CLONE_NEWPID` applies only to children.

5. **Drop creds** in the child. Emulate `nsenter --setuid follow`:
   - Read `/proc/<target-pid>/uid_map` / `gid_map`. Each is a set of
     `inside outside length` triples. The outside of the first "1 inside"
     mapping is the effective uid we want.
     In practice, for Nix's build sandbox the mapping is a single triple
     `0 <build-uid> 1` — the inside-uid-0 maps to the outside build user.
     After entering the user namespace, `setuid(0)` gets us that
     identity inside; but we want the "follow" semantics: match the
     target's current uid as seen INSIDE the namespace. Read
     `/proc/<target-pid>/status`'s `Uid:` line for the canonical answer.
   - `setgroups({})` first (to avoid EPERM on later calls).
   - `setgid(targetGid); setuid(targetUid);` in that order.
   - Confirm via `getuid()`/`getgid()`.

6. **`execvp(bash, args)`** — bash, `--init-file envVarsFileInSandbox`,
   `-i`. Same argv as today.

**Files**:
- `src/nix/debug-attach.cc::CmdDebugAttach::execAttach` — replace the
  `execvp("nsenter", …)` block (~80 lines of new C++).
- `src/nix/debug-attach.cc` — drop any remaining `nsenter`-related
  helpers.
- `tests/nixos/build-debugger.nix` — optionally remove `pkgs.util-linux`
  from the VM's dependencies (only needed it for `nsenter`).

**Depends on**: none.

**Done when**:
- Main VM test's subtest 7 (happy-path attach) passes — same as today;
  this is a substitution, not a behavior change.
- Main VM test's subtest 18 (/proc-check refusal) still passes — pidfd
  adds safety; the cmdline check remains the second line of defense.
- Running the test on a sub-5.3 kernel (unlikely in CI but verify the
  error message locally with `unshare -Urm` simulation) produces the
  clear "requires Linux ≥ 5.3" error.

**References**:
- util-linux's `sys-utils/nsenter.c` — reference impl for cred-follow.
- `pidfd` kernel doc: <https://docs.kernel.org/userspace-api/pid_namespaces.html>.

---

### 06. Shared schema-version constant

**Problem**: `schemaVersion: 1` is hardcoded in three places. A future
bump requires touching all three, risking inconsistency.

**Approach**: extract to `src/libstore/include/nix/store/build-debugger.hh`
(new header):
```cpp
#pragma once
namespace nix {
/// Schema version for files under `<nix-state-dir>/debugger/`.
constexpr int kDebuggerAttachInfoSchemaVersion = 1;
} // namespace nix
```
Include from all three consumers. Remove the local `constexpr int
kDebuggerSchemaVersion = 1;` in `src/nix/debug-attach.cc`.

**Files**:
- New: `src/libstore/include/nix/store/build-debugger.hh`.
- `src/libstore/include/nix/store/meson.build` — add the new header to
  `headers`.
- `src/libstore/unix/build/derivation-builder.cc::publishDebuggerAttachInfo`.
- `src/libstore/build/derivation-building-goal.cc::buildWithBuildHook`
  (redirect writer).
- `src/nix/debug-attach.cc` — replace the local constant, include the
  new header.

**Depends on**: none.

**Done when**:
- `grep -rn schemaVersion src/` finds no remaining numeric literals for
  the debugger schema (all references go through the constant).
- Full build + main VM test still pass.

---

### 07. `ssh-ng://` auto-redirect (needs Store API surface)

**Problem**: `nix build --store ssh-ng://host --build-debugger <drv>` runs
the build on `host` (remote daemon via worker protocol). The remote
daemon instruments the build and publishes attach-info on `host`. But no
redirect attach-info is written on the local client, so
`nix debug-attach <drv>` on the client can't auto-SSH. The user must
pass `--on <host>` explicitly.

The fix is blocked on `Store` not polymorphically exposing its
`StoreReference`; the methods live on `StoreConfig`, which is private to
subclasses.

**Approach**:

1. **Widen `Store`.** In `src/libstore/include/nix/store/store-api.hh`,
   add a virtual method next to `getStoreDir()`:
   ```cpp
       /// Store reference (URI + params). Delegates to the config.
       virtual StoreReference getStoreReference() const = 0;
   ```
2. **Subclass overrides.** Each store subclass holds a `config` already;
   implement `getStoreReference()` as
   `return config->getReference();`. Subclasses: `LocalStore`,
   `RemoteStore` (covers `unix://`, `ssh-ng://` both as variants),
   `LegacySSHStore`, `DummyStore`, `BinaryCacheStore` family,
   `LocalOverlayStore`, etc. Approximately 6–8 subclasses; each a
   one-liner.

3. **Restore the ssh-ng redirect writer** in `src/nix/build.cc::CmdBuild::run`
   (inside the `settings.buildDebugger` block, Linux-guarded):
   ```cpp
   auto ref = store->getStoreReference();
   if (auto * spec = std::get_if<StoreReference::Specified>(&ref.variant);
       spec && spec->scheme == "ssh-ng") {
       auto host = spec->authority;
       // … write <nixStateDir>/debugger/<hash>.attach with remoteHost …
       // (use the same helper as the hook-redirect path; factor into
       // a shared function alongside kDebuggerAttachInfoSchemaVersion.)
   }
   ```

4. **Factor the redirect writer** into a shared helper, since it's now
   used from (a) `derivation-building-goal.cc` (build-hook path) and (b)
   `src/nix/build.cc` (ssh-ng path). Place in
   `src/libstore/include/nix/store/build-debugger.hh` (header) +
   `src/libstore/build-debugger.cc` (impl).

**Files**:
- `src/libstore/include/nix/store/store-api.hh` — add virtual.
- Every Store subclass in `src/libstore/*.cc` — add one-liner override.
- `src/libstore/include/nix/store/build-debugger.hh` — declare
  `void writeDebuggerRedirectAttachInfo(StorePath, std::string_view remoteHost);`.
- `src/libstore/build-debugger.cc` (new) — impl.
- `src/libstore/meson.build` — register the new `.cc`.
- `src/libstore/build/derivation-building-goal.cc` — use the helper in
  place of the inline writer.
- `src/nix/build.cc` — restore the ssh-ng branch.

**Depends on**: `work-item 06` (shared constant lives in the same new
header, so do 06 first).

**Done when**:
- `nix build --store ssh-ng://builder --build-debugger <drv>` writes a
  local redirect.
- `nix debug-attach <drv>` on the client auto-SSHes to `builder`.
- A subtest in `tests/nixos/build-debugger-remote.nix` verifies the above.
- The existing build-hook redirect path behaves identically (uses the
  shared helper; regression-test via `work-item 03` subtest 1).

**Risk**: touching every Store subclass is high blast-radius. Do the
change behind a single commit that flips all subclasses together;
don't leave the build broken mid-PR.

---

### 08. Age-based cleanup of redirect attach-info

**Problem**: `maybeSweepStaleDebuggerAttachInfo` skips redirect-only
entries (no local pid to check liveness on). They're normally cleaned
by `~DerivationBuildingGoal`. But on daemon SIGKILL, the destructor
doesn't fire — redirect leaks indefinitely.

**Approach**: in the sweep, also remove entries older than 6 h regardless
of type. The wrapper itself has a 1 h pause cap, so a 6 h redirect can't
correspond to a live paused build.

**Files**:
- `src/libstore/unix/build/derivation-builder.cc::maybeSweepStaleDebuggerAttachInfo`.
  Add:
  ```cpp
  constexpr auto maxAge = std::chrono::hours(6);
  auto mtime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
      auto sys = std::chrono::file_clock::to_sys(mtime);
      auto age = std::chrono::system_clock::now() - sys;
      if (age > maxAge) {
          std::filesystem::remove(path, rmEc);
          continue;
      }
  }
  ```
  Keep the existing remote-host-preserving logic for younger files.

- `tests/nixos/build-debugger.nix` — forge a redirect with a backdated
  mtime (`touch -d '1 day ago' …`) then run `nix debugger-gc` (or
  trigger the sweep). Verify removal.

**Depends on**: `work-item 01` (test relies on `debugger-gc` for a
deterministic sweep).

**Done when**:
- The new subtest passes.
- The existing redirect-cleanup-via-destructor path still works
  (`work-item 03` subtest 4 still green).

---

### 09. `nix debug-list` output sanitization & hash column

**Problem**: `nix debug-list` prints `drvPath` from the attach-info raw.
A root-forged file could embed ANSI escapes or newlines and corrupt
terminal output. Low impact (root-only) but trivial. Also: users have
no way to see the `<hash>` column that they'd `rm` if they wanted to
force-remove a specific entry.

**Approach**:
- Validate `drvPath` on read: reject if it doesn't match `^/nix/store/[0-9a-z-]+\.drv$`.
- Escape control characters before printing.
- Add a column for the 32-char hash (pulled from the filename, not the
  JSON — which is the trustable source).

**Files**:
- `src/nix/debug-attach.cc::CmdDebugList::run`.
- `src/nix/debug-attach.cc::tryReadAttachInfo` — add drvPath validation.

**Depends on**: none.

**Done when**:
- A forged attach-info with `drvPath = "bogus\033[31m"` is rejected.
- `nix debug-list` output includes the hash column.
- Main VM test's subtest 20 (`nix debug-list`) still passes; extend with
  an assertion that hash appears in output.

---

### 10. `buildDebuggerTarget` path normalization

**Problem**: `shouldApplyBuildDebugger` does string-equality on the drv
path. Paths differing in trailing slash, normalization, or symlink
resolution silently skip instrumentation.

**Approach**: normalize both sides via `std::filesystem::weakly_canonical`
before comparing.

**Files**:
- `src/libstore/unix/build/derivation-builder.cc::shouldApplyBuildDebugger`.
- `tests/nixos/build-debugger.nix` — new subtest that passes the drv
  path with a trailing slash (or via a symlink) to `--build-debugger`
  and verifies the hook still fires.

**Depends on**: none.

**Done when**:
- New subtest passes.
- Main VM test still green end-to-end.

---

### 11. `--timeout` tested against an actively-progressing build

**Problem**: current `--timeout` test only checks against a drv that
never runs. The claim that `--timeout=N` wins even while a real build is
in progress is unverified.

**Approach**: in the main VM test, after the existing timeout subtest,
add:
1. Launch `nix build --build-debugger <long-drv>` in background where
   `<long-drv>` has `buildPhase = "sleep 30; exit 1"`.
2. Immediately run `nix debug-attach --timeout 3 <long-drv>`.
3. Assert the "timed out" error appears within ~5 s (use
   `time.monotonic()` before/after; allow 10 s margin for VM slowness).
4. Wait for background build to finish.

**Files**:
- `tests/nixos/build-debugger.nix` — new subtest.

**Depends on**: none.

**Done when**:
- New subtest passes.

---

### 12. "Force success" escape-hatch test

**Problem**: `doc/manual/source/command-ref/opt-common.md` documents
`trap - EXIT; exit 0` as an escape to force the build to be reported as
successful. Implementation is trusted but untested.

**Approach**: new subtest in the main VM test.
1. Kick off a failing build with `--build-debugger` against a drv
   whose `buildPhase` fails BEFORE creating `$out`.
2. Drive the attach via `script -qc` + piped stdin:
   ```
   mkdir -p "$out"
   echo done > "$out/stamp"
   trap - EXIT
   exit 0
   ```
3. Assert the `nix build` exits 0, and `result/stamp` exists (with
   `--no-link` the link isn't made but the output is valid).

**Files**:
- `tests/nixos/build-debugger.nix` — add the `force-success.nix` test
  fixture and the subtest.

**Depends on**: none.

**Done when**:
- New subtest passes.
- `registerOutputs` doesn't reject the manually-populated `$out` (may
  require checking Nix's output-validity rules; adjust the fixture if
  needed).

---

### 13. `nix debug-list` mixed-state coverage

**Problem**: current listing test covers only "one forged redirect".
Mixed entries (live + stale + redirect) aren't tested.

**Approach**: extend the existing subtest 20. Forge three entries:
- live local pid (point at the test driver's own shell; pre-validate
  with `kill -0`);
- dead local pid (999999999 or similar);
- redirect (`remoteHost: "builder.example"`).

Assert `nix debug-list` prints all three with the correct label
(`local pid N`, `stale`, `dispatched to builder.example`).

**Files**:
- `tests/nixos/build-debugger.nix`.

**Depends on**: none.

**Done when**:
- Extended subtest passes.

---

### 14. `--force` flock rename + clearer messaging

**Problem**: `acquireAttachLock` returns an FD that may or may not hold
the lock depending on `--force`. The function name is misleading.

**Approach**:
- Rename to `openAttachLockFile`.
- Split lock acquisition into a separate `tryAcquireAttachLock` that
  returns a bool (held vs not held).
- Update the `--force` warning to say "not holding the lock; concurrent
  attaches may fight over the PTY".

**Files**:
- `src/nix/debug-attach.cc` — rename, split.

**Depends on**: none.

**Done when**:
- Main VM test's concurrent-flock subtest still passes.
- Warning message is verified in the test (grep for "not holding").

---

### 15. Serve-protocol version-warning test

**Problem**: `src/libstore/legacy-ssh-store.cc` emits a warning when
`--build-debugger` is set but the peer speaks serve protocol < 2.9.
Untested. Will regress if someone bumps the version check.

**Approach**: the only non-flaky way to test this is to run a two-Nix
test where the remote builder is a pinned older Nix (protocol 2.8). The
existing `forNix = nixVersion: runNixOSTest …` hook in `tests/nixos/default.nix`
supports pinning per-test. Add a variant of `build-debugger-remote` that
pins the builder to `pkgs.nixVersions.stable` (or a specific 2.34 that
speaks 2.8) and asserts the warning appears in the client's `nix build`
stderr.

**Caveat**: this depends on a pinned-older-Nix that still ships.
`pkgs.nixVersions.nix_2_23` or similar (check nixpkgs availability).

**Files**:
- `tests/nixos/build-debugger-remote.nix` — add a pinned-older-nix
  parametrization, OR a separate `build-debugger-remote-legacy.nix`.

**Depends on**: `work-item 03`.

**Done when**:
- Running the test with a pinned older Nix produces the warning in the
  client's stderr; the test asserts its presence.
- Removing the warning from `legacy-ssh-store.cc` breaks the test
  (regression-sensitivity confirmed).

---

### 16. Sweep scaling cap

**Problem**: the sweep iterates the full debugger dir every 60 s. On a
heavily-used daemon with thousands of accumulated stale entries, this
is O(n) per publish.

**Approach**: cap per-sweep processing to 1000 entries. Next sweep
(60 s later) picks up the rest; filesystem-directory order is stable
enough that we won't starve specific files for long.

**Files**:
- `src/libstore/unix/build/derivation-builder.cc::maybeSweepStaleDebuggerAttachInfo`
  — add `if (++processed > 1000) break;` inside the loop.

**Depends on**: none.

**Done when**:
- Code change merged. Adding a VM test for this is not worth the runtime
  cost; unit-level it's obvious from the code.

---

### 17. Concurrent-build sweep race test

**Problem**: `maybeSweepStaleDebuggerAttachInfo` uses a static atomic
time guard. Two daemon workers could concurrently pass the guard and
sweep simultaneously; corruption is benign but untested.

**Approach**: new subtest that runs 4 simultaneous `--build-debugger`
builds (on 4 different drvs so they don't share the attach-info slot).
Assert all 4 succeed without any sweep-related corruption (each drv's
expected `.attach` present after publish).

**Files**:
- `tests/nixos/build-debugger.nix`.

**Depends on**: none.

**Done when**:
- New subtest passes.

**Note**: low priority. Skip if it blows out VM-test runtime.

---

### 18. Release-notes entry

**Problem**: build-debugger is a new feature. The `doc/manual/source/release-notes/`
tree must carry an entry at release time.

**Approach**: create `doc/manual/source/release-notes/rl-<next-version>.md`
(or append to an existing one). Short: "New experimental feature
`build-debugger`. Run `nix build --build-debugger <drv>` to pause a
failing build and attach via `nix debug-attach`."

**Files**:
- `doc/manual/source/release-notes/rl-<version>.md`.
- `doc/manual/source/SUMMARY.md.in` — add entry if new file.

**Depends on**: none.

**Done when**:
- Release note present and reviewed.

---

## Dependency graph

```
01 (debugger-gc)  ──┬────────────► 02 (VM split) ───┬──► 03 (two-node remote) ──► 15 (legacy serve warn)
                   │                                │          ▲
                   │                                │          │
                   └──► 08 (age-based sweep)        │     04 (machines SSH config)
                                                    │
                                                    ├──► 10 (target-path norm)         test lives here
                                                    ├──► 11 (--timeout live)           test lives here
                                                    ├──► 12 (force success)            test lives here
                                                    ├──► 13 (debug-list mixed)         test lives here
                                                    ├──► 17 (concurrent race)          test lives here
                                                    └──► … (all test-bearing items)

06 (shared const)  ──► 07 (ssh-ng redirect)

05 (pidfd nsenter)   independent (touches debug-attach.cc only)
09 (debug-list san)  independent
14 (flock rename)    independent
16 (sweep cap)       independent
18 (release notes)   depends on everything else completing
19 (sweep pidfd)     done
20 (exec diagnosis)  done
```

Recommended execution order:
1. **01** (unblocks subtest 17 on the sweep test; small; isolated).
2. **02** (split the monolith so every subsequent test drops into the
   right file). Everything that adds test coverage requires the split
   to exist first; delaying 02 compounds tech debt.
3. **06** (trivial; paves the way for 07).
4. **04** → **03** (two-node remote test needs the machines-config
   lookup to exercise auth realistically).
5. **05** (pidfd nsenter; independent; good to land before 07's larger
   refactor hits the same file).
6. **07** (ssh-ng redirect; high blast radius — Store API widened).
7. **08**, **09**, **10**, **11**, **12**, **13**, **14**, **16**, **17**
   (polish; interleave as convenient — each lands in an already-split
   file).
8. **15** (needs the remote infra from 03).
9. **18** (release notes last).

---

## Code map (function-indexed, not line-indexed)

### Wrapper + goal orchestration
| Concern                   | File                                                        | Symbol                               |
|---------------------------|-------------------------------------------------------------|--------------------------------------|
| wrapper generation        | `src/libstore/unix/build/derivation-builder.cc`             | `generateDebugWrapperScript`         |
| attach-info publish       | `src/libstore/unix/build/derivation-builder.cc`             | `publishDebuggerAttachInfo`          |
| attach-info unpublish     | `src/libstore/unix/build/derivation-builder.cc`             | `unpublishDebuggerAttachInfo`        |
| stale sweep               | `src/libstore/unix/build/derivation-builder.cc`             | `maybeSweepStaleDebuggerAttachInfo`  |
| drv-args parser           | `src/libstore/unix/build/derivation-builder.cc`             | `parseDrvArgsForDebug`               |
| gate check                | `src/libstore/unix/build/derivation-builder.cc`             | `shouldApplyBuildDebugger`           |
| preflight                 | `src/libstore/unix/build/derivation-builder.cc`             | `validateBuildDebuggerPreflight`     |
| redirect write (hook)     | `src/libstore/build/derivation-building-goal.cc`            | `buildWithBuildHook` (inside)        |
| redirect destructor       | `src/libstore/build/derivation-building-goal.cc`            | `~DerivationBuildingGoal`            |
| external-builder refusal  | `src/libstore/build/derivation-building-goal.cc`            | `workImpl` (grep `externalBuilder`)  |
| timeout suppression       | `src/libstore/build/derivation-building-goal.cc`            | `workImpl` (grep `respectTimeouts`)  |

### Daemon / protocol
| Concern                        | File                                                   | Symbol                           |
|--------------------------------|--------------------------------------------------------|----------------------------------|
| trusted-users gate             | `src/libstore/daemon.cc`                               | grep `settings.buildDebugger.name` |
| serve-proto `BuildOptions`     | `src/libstore/include/nix/store/serve-protocol.hh`     | `struct BuildOptions`            |
| serve-proto serialize          | `src/libstore/serve-protocol.cc`                       | `Serialise<BuildOptions>`        |
| serve-proto version            | `src/libstore/include/nix/store/serve-protocol.hh`     | `SERVE_PROTOCOL_VERSION`, `latest` |
| legacy-ssh client settings     | `src/libstore/legacy-ssh-store.cc`                     | `buildSettings` (static)         |
| legacy-ssh version warning     | `src/libstore/legacy-ssh-store.cc`                     | handshake block near `remoteVersion` |
| `nix-store --serve` applier    | `src/nix/nix-store/nix-store.cc`                       | `getBuildSettings` lambda        |

### CLI
| Concern                   | File                                | Symbol                     |
|---------------------------|-------------------------------------|----------------------------|
| `--build-debugger` flag   | `src/nix/build.cc`                  | `CmdBuild::CmdBuild`       |
| CA refusal at CLI         | `src/nix/build.cc`                  | `CmdBuild::run`            |
| `CmdDebugAttach`          | `src/nix/debug-attach.cc`           | `CmdDebugAttach`           |
| `CmdDebugList`            | `src/nix/debug-attach.cc`           | `CmdDebugList`             |
| hash-part parser          | `src/nix/debug-attach.cc`           | `hashPartOf`               |
| SSH dispatch              | `src/nix/debug-attach.cc`           | `CmdDebugAttach::dispatchRemote` |
| in-process attach (→ `05`) | `src/nix/debug-attach.cc`           | `CmdDebugAttach::execAttach` |

### Tests & docs
| Concern                  | File                                                  |
|--------------------------|-------------------------------------------------------|
| main VM test             | `tests/nixos/build-debugger.nix`                      |
| remote VM test (new)     | `tests/nixos/build-debugger-remote.nix` (create)      |
| VM-test registration     | `tests/nixos/default.nix`                             |
| functional test          | `tests/functional/build-debugger.sh`                  |
| functional registration  | `tests/functional/meson.build`                        |
| `--build-debugger` docs  | `doc/manual/source/command-ref/opt-common.md`         |
| `nix debug-attach` docs  | `src/nix/debug-attach.md`                             |

---

## Test discipline

Before any PR:
```bash
# Functional tests: all must pass.
nix build .#nix-everything --no-link -j0

# VM tests: each is a separate derivation; nix schedules them in
# parallel on a multi-core builder. After work-item 02 lands, the
# monolithic `tests.build-debugger` attribute is replaced by the
# list below. Build them as a group:
nix build \
    .#hydraJobs.tests.build-debugger-gates \
    .#hydraJobs.tests.build-debugger-attach \
    .#hydraJobs.tests.build-debugger-bare-bash \
    .#hydraJobs.tests.build-debugger-structured-attrs \
    .#hydraJobs.tests.build-debugger-exit-code \
    .#hydraJobs.tests.build-debugger-security \
    .#hydraJobs.tests.build-debugger-remote \
    --no-link -j0
```

Each must exit 0. Stop and triage if any fail unexpectedly.

When iterating on a single VM test file: build only that one. Full
`nix-everything` and the full VM-test group are gating, not inner-loop.

**Until work-item 02 lands**, the VM tests are still one big
`.#hydraJobs.tests.build-debugger` — run that instead of the list.

---

## Style & conventions

- **Error messages**: no trailing period; use `HintFmt` for interpolation.
- **User-visible warnings**: `logWarning({.msg = HintFmt(…)})`, not
  `printError` (which goes to the build log and is easy to miss).
- **User-visible progress**: `printInfo(…)`.
- **Comments**: explain *why*, not *what*; name the specific constraint
  or invariant the code enforces.
- **Commit messages**: Nix's loose convention is `<area>: <summary>`.
  For this feature, use `build-debugger:` as the area (e.g.
  `build-debugger: race-free attach via pidfd`).
- **No backward-compatibility shims** for the experimental-feature
  surface: the schema version and serve protocol version ARE the
  compatibility mechanisms; don't layer further conditionals on top.

---

## What NOT to do

- **Don't attempt CA derivations or external builders.** Explicitly out
  of scope (confirmed).
- **Don't delete work items or subtests** when they get hard. If you
  can't finish, document the specific blocker in-line in the code AND
  in this file (as an update), referencing the work-item number.
  The previous iteration deleted tasks #89, #96, #97 and that's what
  created this follow-up plan.
- **Don't revert the pivot** (daemon-orchestrated PTY handoff → nsenter
  attach). The pivot is correct; any "this was simpler before" feeling
  is nostalgia, not insight.
- **Don't use `kill(pid, 0)` as a liveness check** anywhere new; use the
  pidfd path (once `work-item 05` lands). Legacy callers: remove them
  too while you're there.
- **Don't add CLI flags named `--no-X`** where the behavior should be
  driven by config (see `work-item 04`'s rejection of `--no-sudo`).
- **Don't cite line numbers as the "location" of something** — they
  drift. Use function/symbol names + grep patterns.
- **Don't use `printError` for things the user needs to see**. Use
  `logWarning` or `printInfo`.

---

## Resolved design decisions

- **Kernel requirement**: Linux ≥ 5.3 (pidfd), ideally ≥ 5.8 (pidfd
  setns). Acceptable per user policy.
- **Concurrent paused builds limit**: no separate knob; `max-jobs`
  already bounds it.
- **SSH auth for `nix debug-attach`**: reuse the `nix.buildMachines`
  entry that the build hook used (`work-item 04`). No `--no-sudo` flag
  — `sudo` is driven by `sshUser == "root"`.

---

## Appendix A: discarded ideas (and why)

- **Rate-limit on concurrent paused builds**: `max-jobs` already bounds
  concurrent builds. Adding a separate knob for the debug subset would
  be complexity without payoff.
- **`--no-sudo` flag**: superseded by the `sshUser`-driven approach in
  `work-item 04`.
- **Fallback to `kill(pid, 0)` on `pidfd_open` ENOSYS**: superseded by
  the "require Linux ≥ 5.3" decision; we error out cleanly instead.
- **Running `nsenter` via user-space tool**: race-prone; see
  `work-item 05`.

---

## Appendix B: handoff meta

- This file is the source of truth for the plan. If the plan changes,
  update this file — don't update task lists externally.
- When you finish a work item, strike it through and add a short note:
  "(done: <commit-sha>, <one-line-summary>)". Keep the content for
  archival reference until the whole plan is closed.
- If you discover a new issue, add it as a new work-item at the end
  (next available number) with the same structure. Update the
  dependency graph.
- If a work item turns out to be wrong or redundant, don't delete it —
  strike it with rationale.

---

## Appendix C: current commit state at handoff

- Branch: `vibe-coding/busybox-breakpoint`
- Serve-protocol version: 2.9
- Attach-info schema version: 1
- VM-test subtests: 21 defined, all in one monolithic file
  (`tests/nixos/build-debugger.nix`). 20 pass; subtest 17 fails,
  blocked on `work-item 01`. `work-item 02` will split this file.
- Functional tests: 211/211 passing.
- `nix-everything` builds clean.

Grep `research/build-debugger-followups.md` references in the tree to
find any `// TODO:` comments that defer to this file. If you resolve
one, remove the comment or re-link it.

---

## Review fixes (2026-04-20)

### Resolved in-place (no new work item)

- **exit-code-passthrough test race** — see status snapshot. Test was
  flaky: signalled the wrapper on attach-info presence (published at
  `startBuild`) instead of env-vars presence (written by the wrapper's
  EXIT trap). SIGTERM during `buildPhase` killed bash 143. Fixed by
  gating the kill on env-vars existence, and by installing the
  TERM/HUP/INT trap at the top of `_nix_debug_attach` rather than
  right before `wait`.
- **`closeExtraFDs(preserve)` dead surface** — signature widening
  reverted. No production caller passed `preserve`; only the 4
  unit tests exercised the new branches. Staged for an earlier
  design iteration that didn't land.
- **`shouldApplyBuildDebugger` repeated canonicalization** — now
  memoised on first call. `buildDebugger{,Target}` don't change over
  a build's lifetime.
- **`kill(pid, 0)` in `debug-attach.cc`** — replaced with
  `pidfdAlive(pidfd)` as required by `What NOT to do §kill(pid,0)`.
  The pidfd is opened before the "paused yet?" check so a single
  open feeds both it and the final pre-fork liveness probe.
- **`publishDebuggerAttachInfo` throwing post-startChild** —
  demoted to warning. The inode-mismatch check is defensive; a
  thrown `Error` after `startChild()` leaked the already-forked
  builder process and corrupted the goal state.
- **Duplicate external-builder refusal** — goal-side check
  deleted; `ExternalDerivationBuilder`'s ctor-side check is
  authoritative.
- **Serve-proto legacy-peer warning dedup** — now keyed per-host
  via `Sync<std::set<std::string>>`, not a single global atomic.
  A daemon with one legacy + one modern builder now gets the
  warning for the legacy one regardless of connection order.
  Regression test still deferred along with work-item 15 (the
  warning requires a pinned older Nix to exercise at all).
- **Sweep uses pidfd-based liveness** — follow-up #19, now closed.
  The sweep's `kill(pid, 0)` was the last legacy caller; consolidated
  on pidfd with `kill` fallback when `pidfd_open` fails.
- **`exec`-chain diagnostic** — follow-up #20, now closed. If the
  wrapper pid is dead and `env-vars` is absent, `nix debug-attach`
  emits a specific "the builder `exec`d away" error instead of the
  generic "wait for the failure" one.
- **`--no-build-debugger` flag** — re-added to `nix build` (erased
  globally by `MixCommonArgs`). Covered by a new subtest in
  `gates.nix`: `--option build-debugger true --no-build-debugger`
  runs the build without the pause banner and without publishing
  attach-info.
- **`--on HOST` bare-ssh fallback test** — new subtest 6 in
  `remote.nix` uses a `/etc/hosts` alias for the builder that
  deliberately is NOT in `nix.buildMachines`. Exercises
  `dispatchRemote`'s "HOST not in machines — use bare ssh"
  fallback path end-to-end.
- **Concurrent-sweep correctness test** — new
  `security-sweep.nix`. Forges dead-pid, ancient-mtime, and
  remoteHost-only entries alongside a live `--build-debugger`
  pause, bounces the daemon to clear the sweep's time guard,
  kicks off a second build, and verifies the sweep:
    - KEEPS the live entry,
    - REMOVES the dead-pid entry (pidfd path),
    - REMOVES the aged entry (age-cull),
    - KEEPS the fresh remoteHost-only entry.

### Resolved follow-up work items (post-review round 2)

#### ~~19. `kill(pid, 0)` still used in sweep liveness~~ (done)

Sweep's per-entry liveness check now prefers `pidfd_open` +
`pidfd_send_signal(fd, 0, …)` over `kill(pid, 0)`. On pre-5.3 kernels
(`pidfd_open` returns -1), falls back to the original `kill` path with
an explanatory comment. No new test; the security-sweep VM test
covers the pidfd-positive path via the forged dead-pid fixture.

#### ~~20. `exec` in sourced builder silently bypasses the debugger~~ (done)

`nix debug-attach` now distinguishes the two failure modes: if the
wrapper pid is dead AND `env-vars` is absent, emit an exec-specific
error naming the limitation instead of the generic "wait for the
failure to appear" message. On pre-5.3 kernels where we can't cheaply
check pidfd liveness, we still emit the old message.

### Resolved follow-up work items (post-review round 3)

#### ~~21. Broaden exec-chain error message~~ (done)

`nix debug-attach` no longer names `exec` as "most likely" cause;
lists three common causes (exec-chain, external kill, daemon
shutdown) with rough probability ordering. Also loosened detection:
"dead pid" now covers both "pidfd opened then ESRCH" and
"pidfd_open returned -1 due to ESRCH" (via `kill(pid, 0)` confirm),
so the diagnostic fires for the common case where the wrapper is
already gone by the time the user runs `debug-attach`.

#### ~~22. Extract pidfd helpers to libutil~~ (done)

`openPidfd` and `pidfdAlive` live in `nix/util/processes.{hh,cc}`
gated on `__linux__`. Syscall-number `#define`s consolidated to
one site. Both existing callers (`debug-attach.cc` and the sweep
in `derivation-builder.cc`) now use the shared helpers.

#### ~~23. Test for exec-chain diagnostic~~ (done)

`tests/nixos/build-debugger/wrapper-exec-chain.nix` forges an
attach-info for a dead pid + absent env-vars and asserts the new
error message wording (names `exec` AND mentions external-kill).

#### ~~24. Test for "no sleep in PATH" wrapper branch~~ (done)

`tests/nixos/build-debugger/wrapper-no-sleep.nix` builds a bare-bash
drv with PATH="" so `command -v sleep` fails in the wrapper.

#### ~~25. Parallel instrumentation scoping test~~ (done)

`tests/nixos/build-debugger/attach-scoping-parallel.nix`: target
drv with two sibling `buildInputs` and `--max-jobs 4`. Asserts the
target's attach-info appears, siblings' don't.

#### ~~26. `--no-build-debugger` flag~~ (done)

Re-added on `nix build` (was erased globally in `MixCommonArgs`).
Gated-feature asymmetry: disabling doesn't require opting into
the feature. Test in `gates.nix` (subtest 22).

#### ~~27. Extended gate tests~~ (done)

Added: nix-build rejection (23), and `--option build-debugger true`
without a target on a succeeding build (24).

#### ~~28. Doc updates~~ (done)

`opt-common.md` documents `--no-build-debugger`. `debug-attach.md`
describes the exec-chain diagnostic.

### New follow-up work items

(None outstanding at this review.)
