---
synopsis: "Nix waits for the builder to exit instead of killing it when it closes its output"
issues: [2176]
---

On Linux, a build now ends only once its builder process has exited as well as closed its standard output and standard error (as before, a process the builder started that still holds them open keeps the build going).
Previously Nix killed the builder as soon as its output was closed.
Builders that close their output and then exit, such as a build script whose last command is a GNU coreutils program (these close standard output at exit), no longer fail intermittently with `signal 9 (Killed)`, and the exit status Nix reports is the builder's own.

A builder that closes its output and keeps running is now subject to [`max-silent-time`](@docroot@/command-ref/conf-file.md#conf-max-silent-time) and [`timeout`](@docroot@/command-ref/conf-file.md#conf-timeout) like any other running builder; without either setting, Nix waits for it.
Processes the builder leaves behind are killed once it has exited, as before.

This uses `pidfd_open()`, available since Linux 5.3.
On older kernels, where a seccomp filter refuses it, and on other platforms, Nix still kills the builder when its output closes.
`nix -v --version` lists `wait-for-builder-exit` among its features when that `nix` process can wait for the builders it starts.
This describes the environment `nix` itself runs in (its kernel and any seccomp filter), not that of a daemon it talks to, which builds in its own.
