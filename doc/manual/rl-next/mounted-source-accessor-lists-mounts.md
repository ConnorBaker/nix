---
synopsis: "Lazily mounted flake inputs are listed in the store directory"
prs: []
---

In impure evaluation, `builtins.readDir` of the store directory now works and lists flake inputs that are mounted but not yet copied to the store, as it did before sources were copied lazily; previously it failed with `path '/' is not a valid store path`.
The evaluator's view of a mount point's parent directory includes the mount point, and the directories leading to a mount point exist even where the underlying filesystem lacks them, so the view behaves as if the copy had been performed.
In pure evaluation, the directories leading to the store directory exist once a store path is allowed: `builtins.pathExists /nix` is true and `builtins.readDir /.` lists `nix`, consistent with `builtins.pathExists /nix/store`, which was already true.
