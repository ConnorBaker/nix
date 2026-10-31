---
synopsis: "Builds without Boehm GC free evaluation memory as they go"
---

A build of Nix without the Boehm garbage collector (`-Dlibexpr:gc=disabled`, or `nix build .#nix-nogc` from Nix's flake) never freed evaluation memory.
Commands that evaluate many independent values, such as the packages of Nixpkgs, held the memory of all of them until they exited.

On 64-bit Linux, such a build now frees that memory during evaluation, without a garbage collector.
Evaluation runs in *jobs*.
While a job runs, Nix records each value that it evaluates in place of an existing unevaluated one.
When the job ends, Nix *reverts* those values to their unevaluated form and frees the memory the job allocated.
What to keep is decided from records made while the values were evaluated; memory is not scanned.
A reverted value is evaluated again when it is next needed.

Some values are kept instead of reverted:
- values that the command still needs;
- values whose evaluation did something that could give another result if done again, such as a download without a hash or a build during evaluation, so that it is not done twice;
- by default, values that an earlier job reverted and a later one needed again, and values that a later element of the same job needs.
  The new setting [`unforce-always-revert`](@docroot@/command-ref/conf-file.md#conf-unforce-always-revert) reverts these too.

These commands run jobs:
- `nix-env`, `nix-instantiate` and `nix-build`, for each derivation they find in a package set, including nested sets marked with `recurseForDerivations`.
  `nix-env` treats each channel of its default expression as a package set of its own.
- `nix-instantiate --eval --strict`, also with `--json` or `--xml`.
- `nix eval`, with or without `--json`, for each element of the value it prints.
- Installables given with `--file` or `--expr`, for the derivations of their attribute paths.
- `nix search`, for each package it visits; `nix flake show`, for each derivation and Hydra job; `nix flake check`, for each output it checks.
- `nix repl`, while it prints a value.
  Values bound in the REPL are never evaluated in a job.

Several consecutive elements may share one job; Nix ends a job between elements when the memory in use has risen past its earlier high point, or when the job has grown larger than any single element did.
Inside a job, memory allocated by the steps of `builtins.foldl'` and by the evaluation of a condition (`if`, `assert`, `&&`, `||`, `->`, `!`) can also be freed as soon as they finish.

These commands no longer hold the memory of every element they evaluate.
Values shared between elements, kept values and memory that a job could not give back remain in use, so how much is saved depends on the command and the expression.
Peak memory is not guaranteed to be lower than in a build with Boehm GC.
Evaluating reverted values again takes time, so evaluation can take more CPU time than in a build with Boehm GC.

Reverting does not change what is evaluated: a successful evaluation produces the same output, and pointer equality, which `==` relies on for values that contain functions, is preserved.
Some diagnostics and failures can differ; their own release notes list them.

Jobs allocate from a large reservation of virtual address space, of which only the pages in use take memory.
If the reservation cannot be made, evaluation fails with an out-of-memory error.

No jobs run, and nothing is reverted:
- in builds with Boehm GC;
- on systems other than Linux, and on 32-bit systems, where a build without Boehm GC still frees no evaluation memory;
- with the debugger (`--debugger`);
- where native code may run during evaluation: with [`allow-unsafe-native-code-during-evaluation`](@docroot@/command-ref/conf-file.md#conf-allow-unsafe-native-code-during-evaluation), when a plugin loaded with [`plugin-files`](@docroot@/command-ref/conf-file.md#conf-plugin-files) adds or replaces builtins, or in a program that uses the C API.

In a build without Boehm GC, [`NIX_SHOW_STATS`](@docroot@/command-ref/env-common.md#env-NIX_SHOW_STATS) now reports an `unforce` object, where a build with Boehm GC reports `gc`.
It counts the jobs that ended (`jobs`) and those kept whole (`jobsKeptWhole`), the values reverted (`cellsReverted`) and kept (`cellsPromoted`), the bytes kept and freed (`bytesKept`, `bytesFreed`), and how often steps of `builtins.foldl'` freed memory and how much (`stepEnds`, `stepBytesFreed`).
If native code may run, `off` gives the reason.
