---
synopsis: "A derivation whose `system` contains a double quote or a backslash is refused at instantiation"
prs: []
---

The store derivation format writes the platform verbatim, so a `system` attribute containing a double quote or a backslash could not be represented; Nix nevertheless wrote such a derivation, and then failed to read it back with "expected string ','" or "unexpected escape sequence in unquoted string".
Instantiating such a derivation now fails with an error naming the platform and the derivation.
