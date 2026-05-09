R""(

# Examples

* Show what was recorded when `nixpkgs#hello` was last evaluated:

  ```console
  # nix eval nixpkgs#hello --eval-trace            # populate the cache
  # nix eval-info nixpkgs#hello --eval-trace
  attr path:        packages.x86_64-linux.hello
  session key:      c1f8a2...
  cached:           yes
  source:           session
  trace id:         17234
  result id:        8891
  trace hash:       19b36c...
  dep-key-set:      9a4487... (id=42)
  result:           FullAttrs (5 entries)
  dependencies:     27
    fileBytes  [node:nixpkgs]
        pkgs/applications/misc/hello/default.nix
        hash=abc123...
    environmentLookup  [<absolute>]
        HOME
        hash=def456...
    ...
  ```

* Dump the full dep set as JSON for further analysis:

  ```console
  # nix eval-info --json nixpkgs#hello --eval-trace | jq '.deps | group_by(.type) | map({type: .[0].type, n: length})'
  [
    {"type": "fileBytes",          "n": 12},
    {"type": "directoryEntries",   "n": 3},
    {"type": "environmentLookup",  "n": 2},
    {"type": "structuredProjection","n": 5},
    ...
  ]
  ```

* Look up a file-eval installable:

  ```console
  # nix eval -f ./default.nix hello --eval-trace
  # nix eval-info -f ./default.nix hello --eval-trace --json
  ```

* Fall back to the History table when the exact session key no longer has
  a row (e.g., after a `flake.lock` change that doesn't invalidate the
  underlying trace):

  ```console
  # nix eval-info --include-history nixpkgs#hello --eval-trace
  ```

# Description

`nix eval-info` reports what the evaluator observed while producing the
value of an [*installable*](./nix.md#installables) — the dependencies
recorded in the eval-trace cache, the cached result, and the runtime
fetch roots associated with the session.

The command reads from the SQLite-backed eval-trace cache only. **It
does not re-evaluate** the attribute being queried. If the cache has no
record for the installable, `nix eval-info` reports a cache miss and
suggests running `nix eval` first.

> **Note**
>
> For flake installables, the command must parse `flake.nix` and lock
> the flake's inputs to derive the same session key that `nix eval`
> would use. This is the same cost as `nix flake metadata`; the flake's
> `outputs` attribute set is *not* evaluated.

# Matching flags with `nix eval`

The session key that identifies a cached evaluation depends on:

- The installable itself (flake ref + attr path, or file + attr path + `--arg`s)
- Policy flags: `--impure`, `--pure-eval`, `--restrict-eval`, `--allowed-uris`
- Evaluator flags: `--eval-trace-hash-algorithm`, `--override-input`, etc.
- The current system (`--system` / `currentSystem`)

`nix eval-info` must be invoked with the same flags that were passed to
`nix eval`, or the lookup will fall into a different session namespace
and report a cache miss. The `--include-history` flag relaxes this to
the *stable recovery namespace*, which tolerates changes to the graph
digest (e.g., a `flake.lock` update) but not to policy flags.

# Output

## Dependencies

The eval-trace cache records a typed dependency for each observation the
evaluator made. The fields reported per dep depend on its kind:

| Kind | What it captures |
|---|---|
| `fileBytes` | Content of a file read by the evaluator |
| `directoryEntries` | Entry set of a directory |
| `existenceCheck` | Whether a path exists |
| `environmentLookup` | Value of an environment variable |
| `sessionSystemValue` | Session-wide constants like `currentSystem` |
| `runtimeFetchIdentity` | Identity of a `builtins.fetchTree` input |
| `derivedStorePath` | A path computed by evaluating a Nix-level path expression |
| `narIdentity` | NAR hash of a copied source tree |
| `structuredProjection` | A specific key or array index inside a JSON / TOML / Nix attrset |
| `implicitStructure` | Structural guard (e.g., `GitRevisionIdentity`) |
| `rawBytes` | Raw literal bytes fed to a builtin |
| `storePathAvailability` | Whether a store path is valid |
| `gitRevisionIdentity` | Git working-dir identity of a flake/file source tree |
| `traceValueContext` | Dependency on another trace's output value |
| `traceParentSlot` | Dependency on a parent attrset's shape |
| `volatileTime` | `builtins.currentTime` — always invalidates |
| `volatileExec` | Output of `exec` — always invalidates |

When a trace contains any volatile dep, it is flagged in the output —
verification always treats such a trace as stale.

## Runtime fetch roots

Flakes can pull inputs at runtime via `builtins.fetchTree`. Each such
fetch is recorded as a *session runtime root* — the fetched store path,
its NAR hash, and the canonical fetch-identity attrset. `nix eval-info`
lists all runtime roots associated with the session, verified against
the current store.

# Exit status

`nix eval-info` exits `0` on both cache hits and clean cache misses.
Non-zero status indicates a usage error (e.g., `eval-trace` disabled),
a cache open failure, or a corrupted record.

)""
