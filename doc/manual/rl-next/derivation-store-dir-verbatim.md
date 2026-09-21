---
synopsis: "A store directory containing a double quote or a backslash is refused when a derivation is written for it"
prs: []
---

The store derivation format writes store paths verbatim under a Unix store directory, so a store directory containing `"` or `\` produced a `.drv` that could not be read back.
Writing a derivation for such a store directory now fails with an error naming the directory and the derivation, as it already does for a `system` containing either character.
A Windows store directory, whose backslashes the format escapes, is unaffected.
