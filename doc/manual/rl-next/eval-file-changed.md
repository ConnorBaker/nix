---
synopsis: "Evaluation stops with exit status 102 if a file it read changes"
---

Where a build without Boehm GC frees evaluation memory in jobs (on 64-bit Linux, see the release note on freeing evaluation memory), a reverted value is evaluated again when it is next needed, and could read a file again after it changed.
So that it never builds a value from the changed file, Nix now records the result of the first read of such a file in a job, as a SHA-256 hash, and checks every later read of the same path by the same builtin against it.

This covers files that can change only by being edited: those outside the Nix store, or in a working tree that Nix reads in place.
The reads checked are:
- `builtins.readFile`, `builtins.hashFile` and `builtins.readDir`;
- the answer of `builtins.pathExists`;
- the file that `builtins.scopedImport` parses;
- where symbolic links lead, for a path copied to the store (such as `"${./file}"` or `builtins.path` without a filter) and for `builtins.storePath`.

If a later read gives another result, leads elsewhere, or fails, evaluation stops with exit status 102 and an error such as:

```
error: file '/home/alice/config.json' changed during evaluation (readFile)
```

Exit status 102 is also the exit status of a build whose output hash does not match.

`builtins.tryEval` does not catch this error, and commands that go on after an error, such as `nix eval` printing `«error: …»`, stop on it.

The check applies to every later read, not only to values evaluated again: an evaluation that reads the same changing file twice now stops, where before each read returned the file's contents at that time.
Files that change without being edited count too, such as those under `/proc` and Nix's own state (for example, the garbage collector roots that `nix-instantiate --add-root` creates between jobs).

`nix repl` does not check reads.
There, values whose evaluation read a file are not reverted, so files that change between commands behave as before.
