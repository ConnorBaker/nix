R""(

# Examples

* Attach to a build paused by `--build-debugger`:

  ```console
  $ nix build nixpkgs#hello --build-debugger
  ...
  build failed in buildPhase with exit code 2
  To attach an interactive shell to the failed build sandbox, run on this host:
      sudo nix debug-attach /nix/store/abcdef…-hello-2.10.drv
  ```

  In another terminal on the same host:

  ```console
  $ sudo nix debug-attach /nix/store/abcdef…-hello-2.10.drv
  # exit   (returns control to `nix build`, which reports the failure)
  ```

# Description

`nix debug-attach` enters the failed build sandbox of a derivation that
is paused by `nix build --build-debugger`, starts an interactive `bash`
shell inside it, and signals the paused build to terminate when the
shell exits.

Must be run as `root` (the sandbox entry uses `setns(2)` on the build
process's Linux namespaces).

The paused build's full post-failure environment — `$out`,
`$NIX_BUILD_TOP`, the failing phase's stdenv variables, and any partial
build artifacts — is restored in the interactive shell via a generated
init file. Analogous in spirit to `breakpointHook` in nixpkgs, with the
key difference that no derivation modification is required: the user
enables the mechanism from the command line.

## Options

* `--on <host>` — run the attach session on `<host>` via SSH instead of
  locally. Use when the build ran on a remote host; the CLI auto-
  follows redirects left by the hook or by `--store ssh-ng://`, so you
  normally don't need to pass `--on`.
* `--force` — attach even when another `nix debug-attach` session holds
  the lock for the same build. The warning reminds you that the two
  sessions will share the PTY.

If the target build is not currently paused at its failure point (no
attach-info yet, or its wrapper hasn't written `env-vars` yet),
`nix debug-attach` fails fast with guidance to wait for the `nix build`
log to print its attach instruction. The CLI does not poll.

When attach-info exists but the wrapper pid has already exited without
writing `env-vars`, the most common cause is an `exec` in the sourced
builder script: `exec` replaces the wrapper's bash with another program
and loses the `EXIT` trap and `failureHook` function. The attach CLI
surfaces this distinctly from the generic "wait for the failure"
message, along with the other common causes (external kill, daemon
shutdown) for context.

## Remote-builder / remote-daemon behaviour

When the paused sandbox lives on a remote host — either because
`nix build --build-debugger` dispatched via `nix.buildMachines` or
because you passed `--store ssh-ng://host` — running `sudo nix
debug-attach <drv>` on the local host automatically SSHes to the
remote and runs the attach there. The SSH credentials come from the
matching `nix.buildMachines` entry (user, key, port) when one exists;
otherwise the invoking user's plain `ssh` config is used.

## Restrictions

* Linux only.
* Requires the experimental feature `build-debugger`.
* CA (content-addressed) and fixed-output derivations are not supported
  — `nix build --build-debugger` refuses these at CLI parse time.
* External-builder derivations are not supported (no in-process sandbox
  to enter).
* Requires Linux ≥ 5.3 on the host that runs the attach (for race-free
  `pidfd`-based process tracking).

)""
