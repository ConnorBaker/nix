---
synopsis: "Local builds on Linux start their build child from a fresh exec of Nix"
prs: []
---

A local build on Linux used to fork the multithreaded Nix process and run Nix code in the fork (sandbox setup, builtin builders) before executing the builder.
A fork of a multithreaded process may only make async-signal-safe calls until it executes another program, and that code allocated.
Nix now starts one *build child server* per goal loop, a fresh exec of Nix with no threads, which forks the build children.
The external builder, the pager and the helper that adds a path to a recursive Nix sandbox are no longer started from such a fork either.

This affects programs that use libnixstore to build locally:

- A dynamically linked program runs the `nix-build-child` helper, which is installed with libnixstore in its libexecdir.
  If it is missing, local builds fail with "this program cannot build locally".
- A statically linked program has no helper.
  It must call `nix_build_child_main(argc, argv)` (C API), or `nix::buildChildMain()`, first in `main()`, before it starts any thread; it then runs itself as the build child server, through `/proc/self/exe`.
  A dynamically linked program may do the same instead of using the helper.
  `nix` does, so its local builds need `/proc/self/exe`.
  The helper does not link mimalloc, which `nix` links by default.
- A builtin builder provided by a plugin works only in programs that pass their plugin loader to `buildChildMain()`, as `nix` does.
  In other programs, such a build fails with "unsupported builtin builder".

The helper and the libraries must be from the same build of Nix as the program that runs them; otherwise the build fails with "the build child server ... is from a different Nix build", as can happen when an upgrade replaces them while the program keeps running.

The external builder now gets only its standard input, output and error; it no longer inherits other file descriptors of Nix.
With `NIX_PAGER` or `PAGER` set, Nix runs only `sh -c` with it, without falling back to `pager`, `less` or `more`, and without either, if none of those is found, the output is not paged.
