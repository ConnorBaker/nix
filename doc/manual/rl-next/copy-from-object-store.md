---
synopsis: "Copying a source the store already holds links its files without reading them"
prs: []
---

When a source is copied into a local store under the git method — a flake input, `builtins.path`, `builtins.fetchGit`, a `path:` input — every directory whose git tree the store already holds is now made from the store's own objects: its files are hard-linked from the object store and its symlinks written from theirs, without reading, serialising or hashing the source.
Only a directory the store does not hold is read, as before.

The case this changes is the copy of an edited checkout, or of any tree that differs from one the store holds in a few files: the cost is one `linkat` per unchanged file instead of a read, a NAR framing and a SHA-256 of it.
The store path, its bytes and its sharing are the same whichever way it was made (the path is checked to dump to the same NAR); a tree object is verified against its identifier as it is read, and a missing or corrupt object sends that directory back to the ordinary route.
Copies through a daemon, into a local overlay store, and on Windows are unchanged.
