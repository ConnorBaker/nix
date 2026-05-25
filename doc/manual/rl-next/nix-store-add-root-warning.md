---
synopsis: "`nix-store --realise` now warns about missing `--add-root` against any store type"
---

`nix-store --realise` previously suppressed the "you did not specify `--add-root`; the result might be removed by the garbage collector" warning whenever the store could not be cast to a `LocalFSStore` (e.g. when realising against a remote daemon, an SSH store, or an HTTP binary cache). `nix-instantiate` already warned in this case, so the two tools disagreed.

Both tools now print the warning regardless of store type, matching the documented intent. The warning text and once-only delivery are unchanged; users who supply `--add-root <path>` see no difference. Users who run `nix-store --realise` against a non-`LocalFSStore` without `--add-root` will see the warning where previously they saw nothing.
