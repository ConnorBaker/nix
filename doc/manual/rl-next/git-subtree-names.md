---
synopsis: "Git sources are hashed once per tree object, and filtered sources of Git inputs are cached"
prs: []
---

A Git input's subtrees are now identified, for the purpose of the source-hashing cache, by their Git tree objects rather than by the input's revision, when the input is fetched without `exportIgnore` (flake inputs, `builtins.fetchTree` with `type = "git"`, and `builtins.fetchGit { exportIgnore = false; }`).
A directory that is unchanged between two revisions is then hashed once, and its store path is known without another walk when the second revision is used, including through `builtins.path` and `builtins.filterSource` of a subdirectory.
With `exportIgnore` (the default of `builtins.fetchGit`), the subtrees are identified the same way as long as no attributes source the repository consults names `export-ignore` (see the release note on `fetchGit` and `export-ignore` attributes), because then the exported tree is the commit's tree; once one does, the contents of a subtree depend on attribute files above it, and the cache works per revision as before.

Filtered sources whose inner tree is so identified, for instance `builtins.path { path = ./.; filter = ...; }` inside a flake, are cached as well: the filter's decisions are recorded as the set of admitted paths, and a later evaluation with the same tree and the same set finds the store path in the cache without reading file contents.

The tarball fetcher no longer keeps a cache of tree hashes to NAR hashes: a tarball's tree is named by its tree hash directly (see the release note on store objects addressed by their git tree hash), so a tarball and a Git input with identical trees share a store path without any cache entry.
