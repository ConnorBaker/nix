---
synopsis: "A NAR entry whose name carries the case-hack suffix is refused"
prs: []
---

On case-insensitive filesystems Nix tells apart directory entries whose names differ only in case by appending a suffix (`~nix~case~hack~<n>`) when it unpacks a [NAR](@docroot@/store/file-system-object/content-address.md#serial-nix-archive), and removes it again when it serialises (`use-case-hack`).
The NAR parser now refuses an entry whose name already carries that suffix, on every platform, with `NAR contains file name '…' with the case-hack suffix '…'`: unpacked where the hack is on, such a name would be read back as a name the archive never had.
The serialiser likewise refuses to dump a directory entry carrying the suffix when the hack is off, since nothing would then strip it.
A NAR written by Nix never contains such a name; one made by another tool that does is now rejected where it was accepted before.
