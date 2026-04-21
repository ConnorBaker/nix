# Release X.Y (202?-??-??)

## New features

- New experimental feature `build-debugger` for interactive post-failure inspection of failed builds

  `nix build --build-debugger <installable>` pauses the sandbox of the specific derivation named on the command line when its build fails, and prints a `nix debug-attach <drv>` command that enters the paused sandbox through Linux namespaces to give the user an interactive `bash` shell with the failed build's environment.

  The paused shell inherits the builder's post-failure state — `$out`, `$NIX_BUILD_TOP`, stdenv phase variables, and partial build artifacts — so the user can inspect what went wrong, re-run failing commands, and optionally populate `$out` manually before letting the build complete.

  Analogous in spirit to [`breakpointHook`](https://nixos.org/manual/nixpkgs/stable/#breakpointhook) in nixpkgs, with the difference that no derivation modification is required — the mechanism is enabled from the command line.

  Supporting command: [`nix debug-attach <drv>`](@docroot@/command-ref/new-cli/nix3-debug-attach.md) enters a paused sandbox. It follows hook-dispatch or `--store ssh-ng://` redirects automatically and SSHes to the remote. `--on <host>` overrides the auto-follow. `--force` attaches even when another session holds the per-build lock. The command does not poll: if the target build is not currently paused at its failure point, it fails fast with guidance to wait for the `nix build` log to print its attach instruction.

  Requirements:

  * Linux only. The attach mechanism enters the failed build's Linux namespaces (`setns(2)`) with a race-free `pidfd`, so a Linux kernel ≥ 5.3 is required.
  * The derivation's builder must be `bash`; content-addressed derivations, fixed-output derivations, and external builders are currently not supported.
  * Clients in daemon mode must be in `trusted-users` — the pause blocks a daemon worker for up to one hour, so untrusted users could otherwise DoS the daemon by stacking paused sandboxes.

  Remote builds dispatched via `nix.buildMachines` or a remote daemon (`--store ssh-ng://host`) publish a redirect on the local host so that running `sudo nix debug-attach <drv>` auto-SSHes to the actual builder and runs the attach session there.
