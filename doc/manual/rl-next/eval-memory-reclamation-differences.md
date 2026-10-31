---
synopsis: "Diagnostics and failures that can differ where evaluation memory is freed in jobs"
---

Where a build without Boehm GC frees evaluation memory in jobs (on 64-bit Linux, see the release note on freeing evaluation memory), a reverted value is evaluated again when it is next needed.
The output of a successful evaluation does not change, but the following can:

- **Traces and warnings.**
  A message from `builtins.trace`, `builtins.warn` or another warning printed during evaluation (such as one about a derivation's attributes) is printed again each time a reverted value that printed it is evaluated again.
  It can therefore appear more than once, and in another order relative to other messages.
  A repeated message can show more of a value evaluated than the first one did, but never a wrong value: for example, `trace: { n = «thunk»; }` followed later by `trace: { n = 4131; }`.
  Messages that the command itself prints, and `error:` messages, are not repeated.

- **Values printed without evaluating them.**
  Error messages and other output that prints a value without evaluating it can show `«thunk»` for a part of the value that was evaluated and has since been reverted.
  What is printed is never wrong, and the value that a message is about always shows at least its outermost form: `[ «thunk» ]`, never just `«thunk»`.
  An error message prints a value with at most 10 attributes and 10 list elements in all.
  Where parts print as `«thunk»`, it can therefore show entries that it would otherwise elide, and then show fewer entries of a later part, followed by `«N items elided»`, or print a later part as `«repeated»` where the same value was printed earlier.

- **Errors.**
  Apart from the call depth differences below, an evaluation that fails does so with the same error, but the frames shown with `--show-trace` can differ, as a failed value can be reverted and evaluated again where it is next needed.
  The message of an error that is evaluated again can show more of a value evaluated, where it would otherwise show `«thunk»`, but never a wrong value.

- **Call depth and stack overflows.**
  A reverted value is evaluated again wherever it is next needed, which can be deeper in function calls or on the native stack than where it was first evaluated.
  - An evaluation that otherwise succeeds can fail on [`max-call-depth`](@docroot@/command-ref/conf-file.md#conf-max-call-depth) or with a stack overflow.
    This includes an error that `builtins.tryEval` would catch becoming a `max-call-depth` error, which it does not catch.
  - Conversely, a `max-call-depth` failure inside a job is not remembered: the value is evaluated again where it is next needed, which can succeed.
    In commands that go on after an error, such as `nix eval` printing `«error: …»` for one attribute, a later use of that value can therefore succeed where it would otherwise fail again.

- **Files changed during evaluation** stop it with exit status 102; see the release note on files changed during evaluation.

`builtins.mapAttrs` in builds without Boehm GC and `nix-env --query --json` have differences of their own, described in their release notes.
