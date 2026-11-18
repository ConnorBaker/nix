R""(

# Examples

* Show statistics about the local store:

  ```console
  # nix store stats
  ```

* Show them as JSON:

  ```console
  # nix store stats --json
  ```

# Description

This command shows statistics about the contents of a Nix store:

* `pathCount`: the number of valid store paths.
* `dedup`: what hard-linking (see `auto-optimise-store` and
  `nix store optimise`) already saves, from the entries in the store's
  `.links` directory:
  * `files`: the number of store files that are hard links to an
    entry, beyond the first file for each entry.
  * `bytes`: the size of those files.
* `predictedDedup`: what `nix store optimise` would add, found by
  hashing every store file that is not yet hard-linked:
  * `files`: the number of files it would replace with a hard link.
  * `bytes`: the size of those files.

With `--json`, the command prints these fields as a JSON object. A
newer store may add fields.

Hashing makes the command take about as long as `nix store optimise`
on the files that are not yet hard-linked.

Local stores and recent versions of the Nix daemon provide
statistics. Other stores, including local overlay stores, do not.

)""
