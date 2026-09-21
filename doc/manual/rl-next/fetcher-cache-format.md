---
synopsis: "The fetcher cache moved to `fetcher-cache-v5.sqlite`, its rows in a compact form"
prs: []
---

The fetchers' cache — the memos of `builtins.fetchGit`, `fetchTree`, `fetchTarball` and the naming of sources — stored every key and value as JSON text, encoded on each write and on each lookup.
It now stores them in a compact length-prefixed form, and lives in `~/.cache/nix/fetcher-cache-v5.sqlite`.
The old `fetcher-cache-v4.sqlite` is not read and may be deleted; the new file is filled again as sources are fetched.
Nothing fetched changes.
