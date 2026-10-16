---
synopsis: "`builtins.findFile` respects pure evaluation and restricted mode when fetching"
---

URLs in a lookup path passed to [`builtins.findFile`](@docroot@/language/builtins.md#builtins-findFile) by Nix code are no longer downloaded in [pure evaluation mode](@docroot@/command-ref/conf-file.md#conf-pure-eval), and in [restricted mode](@docroot@/command-ref/conf-file.md#conf-restrict-eval) they must be allowed by [`allowed-uris`](@docroot@/command-ref/conf-file.md#conf-allowed-uris).
The lookup path configured with `-I`, `NIX_PATH` or `nix-path` is not affected.

In restricted mode, lookup path URLs that are configured or allowed can now be used: accessing the download used to fail with "access to absolute path … is forbidden in restricted mode".

The same goes for `flake:` entries, which must also be locked in pure evaluation mode, as with [`builtins.fetchTree`](@docroot@/language/builtins.md#builtins-fetchTree).
The NAR hash of a `flake:` entry, if specified, is now always verified.

In restricted mode, a URL given as the file to evaluate (e.g. `nix-instantiate --restrict-eval https://…/foo.tar.gz`) can now be used as well.
