---
synopsis: "`builtins.fetchGit` no longer looks up `export-ignore` attributes when the repository declares none"
prs: []
---

`builtins.fetchGit` applies the `export-ignore` attribute by default (`exportIgnore = true`) and, until now, asked libgit2 for the attribute of every path it read, which for a checkout costs a `stat` of `.gitattributes` in every directory above the path, for every path.
It now reads the repository's attribute sources once — the commit's `.gitattributes` files, `info/attributes`, `core.attributesFile` or the user's `~/.config/git/attributes`, the system's `gitattributes`, and the index's and the working directory's `.gitattributes` — and when none of them mentions `export-ignore` it reads the tree without the filter.
What is fetched is unchanged: a repository that declares `export-ignore` anywhere libgit2 would consult is filtered exactly as before.
