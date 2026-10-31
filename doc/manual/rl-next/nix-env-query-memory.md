---
synopsis: "`nix-env --query` uses less memory"
---

`nix-env --query --json` now turns each derivation's JSON into text as soon as it is queried, and keeps only that text until it prints the result, instead of building one JSON document of all derivations.
The output is the same.

`nix-env --query --available` (`-qa`, `-qaP`, `-qac`) no longer keeps the derivations it lists in memory until it prints them, unless an option reads more of them than their names and attribute paths: `--xml`, `--json`, `--system`, `--drv-path`, `--out-path`, `--description`, `--system-filter`, `--status` or `--prebuilt-only`.

Where a build without Boehm GC frees evaluation memory in jobs (on 64-bit Linux, see the release note on freeing evaluation memory), `nix-env -qa --json`, also with `--meta`, queries each derivation as soon as it finds it, instead of after finding all of them, so that the memory of each derivation can be freed.
Its output and exit status are unchanged, and it fails with the same error, but:

- Messages printed while querying (from `builtins.trace` and `builtins.warn`, and `nix-env`'s own `has invalid meta attribute` and `skipping derivation` warnings) appear in the order the derivations are found, interleaved with the messages printed while finding them, instead of in name order after them.
- If a query fails:
  - messages from derivations that would not have been queried can appear before the error;
  - the error message can show more of a value evaluated, but never a wrong value;
  - its trace can include frames of another derivation's query that evaluated the failing value first, such as `… while querying the derivation named 'y-1.0'`, and without `--show-trace` it can stop before the frames it would otherwise show;
  - derivations that would not have been queried before the error can now be queried, so if such a query does not terminate, or crashes, `nix-env` does too.
- Queries run at least one function call level deeper, so [`max-call-depth`](@docroot@/command-ref/conf-file.md#conf-max-call-depth) can be reached sooner.

Wherever no jobs run (in builds with Boehm GC, on systems other than 64-bit Linux, with the debugger, or where native code may run during evaluation), and with `--system-filter`, `--status` or `--prebuilt-only`, the queries run after finding all derivations, as before.
