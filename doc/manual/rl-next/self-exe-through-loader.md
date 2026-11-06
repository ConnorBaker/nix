---
synopsis: Nix started through the dynamic loader finds its own program
---

When Nix is started through the dynamic loader (`ld.so /path/to/nix ...`), `/proc/self/exe` is the loader, which Nix took for its own program.
Nix now finds its program in that case and runs it through the same loader, so that the interpreter named in the program need not exist, as its default build hook (`ld.so /path/to/nix __build-remote`), as the daemon's workers, as the `nix-env` that `nix-channel` runs, and as the chroot helper of `nix run` and `nix shell`.
This uses the loader's `--argv0` option (glibc 2.33).
The loader opens the program again by name each time, so renaming or deleting the running program makes the next of these fail (exit status 127); started directly, Nix keeps running `/proc/self/exe`, which survives that.
Nix reads the program's name from `/proc/self/map_files`, where a deleted program's name ends in " (deleted)"; if that cannot be read, Nix takes the loader for its program, as before.
Before, a daemon started through the loader failed every connection, `nix-channel` and the chroot helper of `nix run` and `nix shell` failed, and, depending on timing, a build through the loader either failed with "unexpected EOF reading a line" from the build hook, or ran locally after "build hook died unexpectedly", never using remote builders.
