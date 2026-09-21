---
synopsis: "Stabilisation of the `git-hashing` experimental feature"
prs: []
---

The `git-hashing` experimental feature has been stabilised and removed.
The [`git` content-address method](@docroot@/store/store-object/content-address.md#method-git) is how the store names every store object and every source, and a method that names everything cannot be optional; `outputHashMode = "git"` on derivations and the reading of Git object streams, which the feature still gated, need no feature either.

A configuration that still lists it (`experimental-features = git-hashing`) gets a one-line warning that the feature has been stabilized, and is otherwise accepted.
The default `outputHashMode` of fixed-output derivations is unchanged (`flat`).
The stable method admits SHA-256 only; the SHA-1 form the feature also admitted is removed (see the release note on the one address for what is refused and the remedy).
