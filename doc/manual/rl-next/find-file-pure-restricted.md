---
synopsis: "`builtins.findFile` respects pure evaluation and restricted mode when fetching"
---

URLs in a lookup path passed to [`builtins.findFile`](@docroot@/language/builtins.md#builtins-findFile) by Nix code are no longer downloaded in [pure evaluation mode](@docroot@/command-ref/conf-file.md#conf-pure-eval), and in [restricted mode](@docroot@/command-ref/conf-file.md#conf-restrict-eval) they must be allowed by [`allowed-uris`](@docroot@/command-ref/conf-file.md#conf-allowed-uris).
The lookup path configured with `-I`, `NIX_PATH` or `nix-path` is not affected.
