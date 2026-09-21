---
synopsis: "A `path:` flake input outside the store is no longer copied again when it is unchanged"
prs: []
---

A `path:` flake reference or input that is not itself a store path — `path:/some/checkout`, or `inputs.x.url = "path:../x"` — was dumped into the store on every evaluation, restoring the whole tree before the store could tell that it already held it.
Nix now hashes the tree first and copies it only when the store lacks it; a second evaluation of an unchanged tree reads it once and writes nothing.
An edited tree is copied as before, and the `lastModified` attribute is unchanged.
