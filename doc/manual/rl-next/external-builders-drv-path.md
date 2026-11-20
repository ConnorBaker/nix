---
synopsis: "External builders: `drvPath` in `build.json`"
issues: []
prs: []
---

The JSON document passed to an [external builder](@docroot@/command-ref/conf-file.md#conf-external-builders) now has a `drvPath` field that identifies the derivation being built.
The derivation need not be present in the store.
