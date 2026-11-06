---
synopsis: Nix started through the dynamic loader finds its own program
---

When Nix is started through the dynamic loader (`ld.so /path/to/nix ...`), `/proc/self/exe` is the loader, which Nix took for its own program.
Nix now finds its program in that case, so its default build hook (`nix __build-remote`) runs Nix rather than the loader.
Before, depending on timing, a build through the loader either failed with "unexpected EOF reading a line" from the build hook, or ran locally after "build hook died unexpectedly", never using remote builders.
`nix run` and `nix shell` on a store whose logical and physical directories differ run their chroot helper from that path too; the daemon's workers still run `/proc/self/exe`, which is then the loader.
