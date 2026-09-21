---
synopsis: "`nix-prefetch-url -A` now honours the `name` attribute of the fetched derivation"
prs: []
---

`nix-prefetch-url --attr` read the `name` attribute of the selected fetcher call under an inverted condition: when the attribute was present it was ignored, so the store path was named after the URL, and when it was absent the command crashed.
It now uses the attribute when present and falls back to the URL's base name otherwise, as documented.
