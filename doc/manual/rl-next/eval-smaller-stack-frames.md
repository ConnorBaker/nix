---
synopsis: "The evaluator uses less native stack in deep recursion"
---

Many functions on the evaluator's recursive paths now keep what they rarely need (errors, traces, debugger support, temporaries) out of their stack frames, and reading or copying files no longer puts 64 KiB buffers on the stack.
Recursion through function calls, `//`, `?`, `builtins.deepSeq`, derivations that depend on other derivations, and the printing of nested values (`nix eval`, `--json`, `--xml`) therefore takes less native stack per level, so deeper recursions fit in the same stack.

How deep a particular expression can recurse before it overflows the stack is not guaranteed, and differs between builds and compilers.
