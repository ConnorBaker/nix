---
synopsis: "Several primops now anchor diagnostics at the offending attribute rather than the call site"
---

`builtins.fetchurl`, `builtins.fetchTarball`, `builtins.fetchGit`, and `builtins.path` previously emitted diagnostics anchored at the primop call site (e.g. the position of the `builtins.fetchurl` invocation). They now anchor at the position of the attribute that caused the error (e.g. the `name` attribute of the argument set), matching the behaviour of `builtins.fetchTree` and friends.

The primop name in the diagnostic header was also previously baked into the format string and rendered without ANSI colouring. It is now interpolated as `'%s'`, picking up the same magenta/uncolored treatment other primop-name references receive in error messages.

Existing diagnostics are otherwise unchanged in wording.
