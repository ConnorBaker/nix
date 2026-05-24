---
synopsis: GitHub fetcher now validates URL parameters
prs: [15331]
issues: [15304]
---

The `github:` fetcher (and the related `gitlab:` and `sourcehut:` fetchers) now validate URL parameters and will error if an invalid parameter is provided. Previously-silent attributes that now error at parse:

- `tag` (the original case from the linked PR/issue)
- `treeHash` (an abandoned scaffolding for upstream-tree-hash propagation that was never wired up; removed alongside the validation tightening)
