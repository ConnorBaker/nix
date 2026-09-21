---
synopsis: "`builtins.fetchGit` of a revision takes `export-ignore` attributes from the commit, not from the checkout"
prs: []
---

The `export-ignore` attribute that `builtins.fetchGit` applies by default decides what a *tree* exports, and for `builtins.fetchGit { url; rev; }` that tree is the commit's.
The attributes consulted for a revision are now the commit's own `.gitattributes` files and the repository's `$GIT_DIR/info/attributes`, together with `core.attributesFile` (or `~/.config/git/attributes`) and the system's `gitattributes` — what `git archive <rev>` reads when it is not given `--worktree-attributes`.

Until now libgit2 was asked with its default source order, which reads the working directory's `.gitattributes` and then the index's entry *before* the commit's blob.
So an uncommitted or staged `.gitattributes` in a checkout could decide what a fetch of a committed revision from that checkout exported, and the same revision fetched from a bare clone exported something else.
That no longer happens: a revision fetched from a checkout and from a bare clone of it now give the same tree.

Fetching a working directory — `builtins.fetchGit` of a local path with no `rev` — is unchanged: there the tree named is the working tree, and its own `.gitattributes` are part of it.
