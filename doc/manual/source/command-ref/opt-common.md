<!-- Some of the options documented here are hardcopied from
     src/libcmd/common-eval-args.cc
-->

# Common Options

Most Nix commands accept the following command-line options:

- <span id="opt-help">[`--help`](#opt-help)</span>

  Prints out a summary of the command syntax and exits.

- <span id="opt-version">[`--version`](#opt-version)</span>

  Prints out the Nix version number on standard output and exits.

- <span id="opt-verbose">[`--verbose`](#opt-verbose)</span> / `-v`

  Increases the level of verbosity of diagnostic messages printed on standard error.
  For each Nix operation, the information printed on standard output is well-defined;
  any diagnostic information is printed on standard error, never on standard output.

  This option may be specified repeatedly.
  Currently, the following verbosity levels exist:

  - `0` “Errors only”

    Only print messages explaining why the Nix invocation failed.

  - `1` “Informational”

    Print *useful* messages about what Nix is doing.
    This is the default.

  - `2` “Talkative”

    Print more informational messages.

  - `3` “Chatty”

    Print even more informational messages.

  - `4` “Debug”

    Print debug information.

  - `5` “Vomit”

    Print vast amounts of debug information.

- <span id="opt-quiet">[`--quiet`](#opt-quiet)</span>

  Decreases the level of verbosity of diagnostic messages printed on standard error.
  This is the inverse option to `-v` / `--verbose`.

  This option may be specified repeatedly.
  See the previous verbosity levels list.

- <span id="opt-log-format">[`--log-format`](#opt-log-format)</span> *format*

  This option can be used to change the output of the log format, with *format* being one of:

  - `raw`

    This is the raw format, as outputted by nix-build.

  - `internal-json`

    Outputs the logs in a structured manner.

    > **Warning**
    >
    > While the schema itself is relatively stable, the format of
    > the error-messages (namely of the `msg`-field) can change
    > between releases.

  - `bar`

    Only display a progress bar during the builds.

  - `bar-with-logs`

    Display the raw logs, with the progress bar at the bottom.

- <span id="opt-no-build-output">[`--no-build-output`](#opt-no-build-output)</span> / `-Q`

  By default, output written by builders to standard output and standard error is echoed to the Nix command's standard error.
  This option suppresses this behaviour.
  Note that the builder's standard output and error are always written to a log file in `prefix/nix/var/log/nix`.

- <span id="opt-max-jobs">[`--max-jobs`](#opt-max-jobs)</span> / `-j` *number*

  Sets the maximum number of build jobs that Nix will perform in parallel to the specified number.
  Specify `auto` to use the number of CPUs in the system.
  The default is specified by the `max-jobs` configuration setting, which itself defaults to `1`.
  A higher value is useful on SMP systems or to exploit I/O latency.

  Setting it to `0` disallows building on the local machine, which is useful when you want builds to happen only on remote builders.

- <span id="opt-cores">[`--cores`](#opt-cores)</span>

  Sets the value of the `NIX_BUILD_CORES` environment variable in the invocation of builders.
  Builders can use this variable at their discretion to control the maximum amount of parallelism.
  For instance, in Nixpkgs, if the derivation attribute `enableParallelBuilding` is set to `true`, the builder passes the `-jN` flag to GNU Make.
  It defaults to the value of the `cores` configuration setting, if set, or `1` otherwise.
  The value `0` means that the builder should use all available CPU cores in the system.

- <span id="opt-max-silent-time">[`--max-silent-time`](#opt-max-silent-time)</span>

  Sets the maximum number of seconds that a builder can go without producing any data on standard output or standard error.
  The default is specified by the `max-silent-time` configuration setting.
  `0` means no time-out.

- <span id="opt-timeout">[`--timeout`](#opt-timeout)</span>

  Sets the maximum number of seconds that a builder can run.
  The default is specified by the `timeout` configuration setting.
  `0` means no timeout.

- <span id="opt-keep-going">[`--keep-going`](#opt-keep-going)</span> / `-k`

  Keep going in case of failed builds, to the greatest extent possible.
  That is, if building an input of some derivation fails, Nix will still build the other inputs, but not the derivation itself.
  Without this option, Nix stops if any build fails (except for builds of substitutes), possibly killing builds in progress (in case of parallel or distributed builds).

- <span id="opt-keep-failed">[`--keep-failed`](#opt-keep-failed)</span> / `-K`

  Specifies that in case of a build failure, the temporary directory (usually in `/tmp`) in which the build takes place should not be deleted.
  The path of the build directory is printed as an informational message.

- <span id="opt-build-debugger">[`--build-debugger`](#opt-build-debugger)</span>

  On a failing build of the **specific installable** named on the command line, pause the failed sandbox and print instructions for attaching an interactive `bash` shell to it via a separate [`nix debug-attach`](@docroot@/command-ref/new-cli/nix3-debug-attach.md) invocation.
  The attached shell inherits the builder's post-failure environment — `$out`, `$NIX_BUILD_TOP`, stdenv phase variables, partial build artifacts — so you can inspect what went wrong and re-run failing commands manually without modifying the derivation.
  Analogous in spirit to [`breakpointHook`](https://nixos.org/manual/nixpkgs/stable/#ssec-stdenv-hooks-breakpoint) in nixpkgs, but works on any stdenv-style derivation without re-expressing the package.

  **Workflow.** After a failing build prints the attach instructions, run `sudo nix debug-attach <drv>` in another terminal. The command enters the sandbox's Linux namespaces (via `setns(2)` on a `pidfd`-pinned handle to the build process) and execs an interactive `bash`. When you exit the shell, the paused build is signaled to terminate and is reported as failed in the normal way, with the original builder exit code. The sandbox directory is preserved (as with [`--keep-failed`](#opt-keep-failed)) for further inspection.

  **Scoping.** The hook is applied only to the one derivation the user is building.
  Dependencies in the closure build normally — if they fail, the failure propagates but the debug pause is **not** triggered.
  This keeps the semantics predictable and lets you use `--build-debugger` alongside `--max-jobs > 1` and `--keep-going`; the parallelism affects only the dependency graph, not the targeted derivation.

  **Restrictions:**

  - Linux only; depends on Linux namespaces + `setns(2)` (requires Linux ≥ 5.3 for `pidfd_open` on the attach side).
  - Requires the experimental feature [`build-debugger`](@docroot@/contributing/experimental-features.md#xp-feature-build-debugger).
  - Only accepted on `nix build`; other subcommands (`nix develop`, `nix shell`, `nix run`, `nix-build`) reject the flag.
  - Must target exactly one installable on the command line.
  - In daemon mode, the calling user must be in [`trusted-users`](@docroot@/command-ref/conf-file.md#conf-trusted-users). The pause blocks a daemon worker for up to an hour; untrusted users could otherwise DoS the daemon by stacking paused sandboxes.
  - The `nix debug-attach` invocation must run as `root` on the host that is actually running the build. When the build ran remotely (via `--store ssh-ng://host` or a `nix.buildMachines` dispatch) `nix debug-attach <drv>` on the local host follows the redirect recorded at dispatch time and SSHes to the remote automatically.
  - Only works when the derivation's `builder` resolves to `bash`; refused for builders that invoke `-c <inline>`, use non-`bash` interpreters, or pass unsupported bash options (anything other than `-e`/`-u`/`-x`/`-o pipefail`).
  - Refused for external builders (derivations dispatched via the `external-builders` experimental feature).
  - Refused for content-addressed / fixed-output derivations (at `nix build --build-debugger` parse time): CA resolution renames the derivation at build time and the target-path comparison doesn't follow the rename.
  - Only fires on builder-process failure (non-zero exit from the builder). Output-verification failures (wrong hash in a fixed-output derivation, missing output, post-build check failures) happen after the builder has already exited successfully and do **not** trigger the debugger.
  - When the user exits the debug shell without an explicit nonzero exit code (plain `exit` or `exit 0`), the build is reported with the *original* builder failure code — not 0. An explicit `exit N` with `N > 0` is passed through unchanged. The debug shell cannot flip a failed build to successful: the wrapper always reports the original failure once the attached shell exits. To produce a successful build from a failed-build artifact you populated manually, re-run the derivation with the output bundled in (for example, via a `fetchurl`/`fetchzip` referencing your populated `$out`).
  - The build log and `nix log <drv>` reflect the state at the moment of failure; commands typed into the debug shell are **not** recorded.
  - Mutually exclusive with [`breakpointHook`](https://nixos.org/manual/nixpkgs/stable/#ssec-stdenv-hooks-breakpoint): the hook's `sleep` fires before this flag's `EXIT` trap and would suppress the debug pause. Remove `breakpointHook` from the derivation when using `--build-debugger`.
  - Derivations that install their own `EXIT` trap after the wrapper sources the builder script will clobber the debugger's `EXIT` hook. `stdenv.mkDerivation` is handled specially — its `exitHandler` calls `runHook failureHook`, and the wrapper defines `failureHook` as a function so the attach still fires. Non-stdenv scripts that set `trap '...' EXIT` themselves will silently bypass the debugger.
  - `exec` in the sourced builder script replaces the wrapper's bash with another program, losing the EXIT trap and `failureHook` function. `--build-debugger` cannot intercept `exec` chains; if the exec'd program fails, no debug shell attaches.
  - After an hour of paused waiting with no attach, the wrapper gives up and lets the build fail in the normal way.
  - Mutually exclusive with [`--repair`](#opt-repair) and [`--rebuild`](#opt-rebuild) / [`--check`](#opt-check). Both repair and rebuild compare output hashes rather than interpret builder exit status, so the debug wrapper (which fires only on a non-zero builder exit) would add no useful signal in those modes. `nix build --build-debugger --repair` and `nix build --build-debugger --rebuild` are refused at CLI parse time.

  **Overriding the setting per-invocation.** The flag is equivalent to `--option build-debugger true`. To force the debugger OFF for a single invocation when the `build-debugger` setting is enabled in `nix.conf`, pass `--no-build-debugger` (a subcommand-local flag on `nix build`; not gated by the `build-debugger` experimental feature, so disabling is always available).

  **Supported execution contexts.**

  *"Remote builder" and "remote daemon" are two different things:*
  - A **remote daemon** (`--store ssh-ng://host`) means your entire daemon lives on the far side of an SSH connection — every store operation is remote.
  - A **remote builder** (`nix.buildMachines = [ … ]`) means your local daemon orchestrates, but schedules specific derivations off-box when their `system`/`requiredSystemFeatures` don't match locally (or when `max-jobs = 0` forces everything remote). Derivations that can build locally still do.

  | Scenario | Status |
  |---|---|
  | Local daemon (default `nix-daemon` via unix socket) | **Supported**, covered by end-to-end tests. |
  | Local-store mode (no daemon, e.g. `nix build --store local:/path`) | **Supported**. Same mechanism as daemon mode. |
  | Remote daemon via `--store ssh-ng://host` | **Supported.** The local `nix build` writes a redirect file so `sudo nix debug-attach <drv>` on the local host auto-SSHes to the remote and runs the attach session there. |
  | Remote builder via `nix.buildMachines = [ … ]` — **when the targeted drv itself goes off-box** | **Supported.** The build hook's reply populates a local redirect; `sudo nix debug-attach <drv>` on the local host SSHes to the chosen builder using the same `sshUser`/`sshKey` the hook used. |
  | Remote builder via `nix.buildMachines = [ … ]` — **when the targeted drv builds locally** (typical case when the drv's system matches the local host) | **Supported.** Only the targeted drv gets the hook; dependencies can still go off-box via the build hook. |
  | Content-addressed derivation (`__contentAddressed = true`) | **Not supported.** Refused at `nix build --build-debugger` parse time. Workaround: remove `__contentAddressed` while debugging, or target the resolved derivation path directly. |
  | External builder (`external-builders` experimental feature) | **Not supported.** `ExternalDerivationBuilder` refuses on construction — its JSON-over-stdin dispatch has no seam for the wrapper script. |

- <span id="opt-fallback">[`--fallback`](#opt-fallback)</span>

  Whenever Nix attempts to build a derivation for which substitutes are known for each output path, but realising the output paths through the substitutes fails, fall back on building the derivation.

  The most common scenario in which this is useful is when we have registered substitutes in order to perform binary distribution from, say, a network repository.
  If the repository is down, the realisation of the derivation will fail.
  When this option is specified, Nix will build the derivation instead.
  Thus, installation from binaries falls back on installation from source.
  This option is not the default since it is generally not desirable for a transient failure in obtaining the substitutes to lead to a full build from source (with the related consumption of resources).

- <span id="opt-readonly-mode">[`--readonly-mode`](#opt-readonly-mode)</span>

  When this option is used, no attempt is made to open the Nix database.
  Most Nix operations do need database access, so those operations will fail.

- <span id="opt-arg">[`--arg`](#opt-arg)</span> *name* *value*

  This option is accepted by `nix-env`, `nix-instantiate`, `nix-shell` and `nix-build`.
  When evaluating Nix expressions, the expression evaluator will automatically try to call functions that it encounters.
  It can automatically call functions for which every argument has a [default value](@docroot@/language/syntax.md#functions) (e.g., `{ argName ?  defaultValue }: ...`).

  With `--arg`, you can also call functions that have arguments without a default value (or override a default value).
  That is, if the evaluator encounters a function with an argument named *name*, it will call it with value *value*.

  For instance, the top-level `default.nix` in Nixpkgs is actually a function:

  ```nix
  { # The system (e.g., `i686-linux') for which to build the packages.
    system ? builtins.currentSystem
    ...
  }: ...
  ```

  So if you call this Nix expression (e.g., when you do `nix-env --install --attr pkgname`), the function will be called automatically using the value [`builtins.currentSystem`](@docroot@/language/builtins.md) for the `system` argument.
  You can override this using `--arg`, e.g., `nix-env --install --attr pkgname --arg system \"i686-freebsd\"`.
  (Note that since the argument is a Nix string literal, you have to escape the quotes.)

- <span id="opt-arg-from-file">[`--arg-from-file`](#opt-arg-from-file)</span> *name* *path*

  Pass the contents of file *path* as the argument *name* to Nix functions.

- <span id="opt-arg-from-stdin">[`--arg-from-stdin`](#opt-arg-from-stdin)</span> *name*

  Pass the contents of stdin as the argument *name* to Nix functions.

- <span id="opt-argstr">[`--argstr`](#opt-argstr)</span> *name* *value*

  This option is like `--arg`, only the value is not a Nix expression but a string.
  So instead of `--arg system \"i686-linux\"` (the outer quotes are to keep the shell happy) you can say `--argstr system i686-linux`.

- <span id="opt-attr">[`--attr`](#opt-attr)</span> / `-A` *attrPath*

  Select an attribute from the top-level Nix expression being evaluated.
  (`nix-env`, `nix-instantiate`, `nix-build` and `nix-shell` only.)
  The *attribute path* *attrPath* is a sequence of attribute names separated by dots.
  For instance, given a top-level Nix expression *e*, the attribute path `xorg.xorgserver` would cause the expression `e.xorg.xorgserver` to be used.
  See [`nix-env --install`](@docroot@/command-ref/nix-env/install.md) for some concrete examples.

  In addition to attribute names, you can also specify array indices.
  For instance, the attribute path `foo.3.bar` selects the `bar`
  attribute of the fourth element of the array in the `foo` attribute
  of the top-level expression.

- <span id="opt-eval-store">[`--eval-store`](#opt-eval-store)</span> *store-url*

  The [URL to the Nix store](@docroot@/store/types/index.md#store-url-format) to use for evaluation, i.e. where to store derivations (`.drv` files) and inputs referenced by them.

- <span id="opt-expr">[`--expr`](#opt-expr)</span> / `-E`

  Interpret the command line arguments as a list of Nix expressions to be parsed and evaluated, rather than as a list of file names of Nix expressions.
  (`nix-instantiate`, `nix-build` and `nix-shell` only.)

  For `nix-shell`, this option is commonly used to give you a shell in which you can build the packages returned by the expression.
  If you want to get a shell which contain the *built* packages ready for use, give your expression to the `nix-shell --packages ` convenience flag instead.

- <span id="opt-I">[`-I` / `--include`](#opt-I)</span> *path*

  Add an entry to the list of search paths used to resolve [lookup paths](@docroot@/language/constructs/lookup-path.md).
  This option may be given multiple times.

  Paths added through `-I` take precedence over the [`nix-path` configuration setting](@docroot@/command-ref/conf-file.md#conf-nix-path) and the [`NIX_PATH` environment variable](@docroot@/command-ref/env-common.md#env-NIX_PATH).

- <span id="opt-impure">[`--impure`](#opt-impure)</span>

  Allow access to mutable paths and repositories.

- <span id="opt-option">[`--option`](#opt-option)</span> *name* *value*

  Set the Nix configuration option *name* to *value*.
  This overrides settings in the Nix configuration file (see nix.conf5).

- <span id="opt-repair">[`--repair`](#opt-repair)</span>

  Fix corrupted or missing store paths by redownloading or rebuilding them.
  Note that this is slow because it requires computing a cryptographic hash of the contents of every path in the closure of the build.
  Also note the warning under `nix-store --repair-path`.

> **Note**
>
> See [`man nix.conf`](@docroot@/command-ref/conf-file.md#command-line-flags) for overriding configuration settings with command line flags.
