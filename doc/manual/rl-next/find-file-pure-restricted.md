---
synopsis: "`builtins.findFile` respects pure evaluation and restricted mode when fetching"
---

URLs in a lookup path passed to [`builtins.findFile`](@docroot@/language/builtins.md#builtins-findFile) by Nix code are no longer downloaded in [pure evaluation mode](@docroot@/command-ref/conf-file.md#conf-pure-eval), and in [restricted mode](@docroot@/command-ref/conf-file.md#conf-restrict-eval) they must be allowed by [`allowed-uris`](@docroot@/command-ref/conf-file.md#conf-allowed-uris).
The lookup path configured with `-I`, `NIX_PATH` or `nix-path` is not affected.

In restricted mode, lookup path URLs that are configured or allowed can now be used: accessing the download used to fail with "access to absolute path … is forbidden in restricted mode".

`flake:` entries passed to `builtins.findFile` by Nix code are now restricted as well: in pure evaluation mode they must be locked, and in restricted mode they must be allowed by `allowed-uris`.
This is stricter than [`builtins.fetchTree`](@docroot@/language/builtins.md#builtins-fetchTree): an entry that is locked only by its NAR hash is refused in pure evaluation mode.
A NAR hash given in a `flake:` entry, or in the flake registry entry it resolves to, is now verified; it used to be ignored for `path:` flakes and for indirect references such as `flake:nixpkgs?narHash=…`.
