---
synopsis: "`builtins.mapAttrs` allocates less in builds without Boehm GC"
---

In a build without Boehm GC, each attribute of the result of `builtins.mapAttrs f attrs` no longer allocates a separate value for `f name`.
When the attribute is evaluated, `f` is applied to the attribute's name and the result to its value.
Values, and pointer equality in Nix expressions, are unchanged, but some details differ:

- If an attribute of the result is evaluated again while its own evaluation is still running, as for an attribute that depends on itself until [`max-call-depth`](@docroot@/command-ref/conf-file.md#conf-max-call-depth) is reached, `f` is applied to the name again each time.
  A `builtins.trace` in the part of `f` that is evaluated when it is applied to the name is then printed once per such evaluation, instead of once.
- `f name` is evaluated one function call level shallower than before, so it can reach `max-call-depth` later, never sooner.
- [`NIX_SHOW_STATS`](@docroot@/command-ref/env-common.md#env-NIX_SHOW_STATS) counts one value fewer per attribute.
- In the C API, each copy that `nix_copy_value` makes of an attribute that has not been evaluated yet applies `f` to the name itself.
  Values that only this application creates, such as a function that it returns, are then distinct in each copy: comparing two such copies with `==` can give `false` where it gave `true` before, and side effects of `f name`, such as traces, happen once per copy.

With [`trace-function-calls`](@docroot@/command-ref/conf-file.md#conf-trace-function-calls) or an evaluation profiler ([`eval-profiler`](@docroot@/command-ref/conf-file.md#conf-eval-profiler)) enabled, and in builds with Boehm GC, `builtins.mapAttrs` behaves as before.
