R""(

# Examples

* Evaluate an attribute set of derivations to JSON:

  ```console
  # nix eval-drvs github:NixOS/nix#checks.x86_64-linux
  ```

* Evaluate an attribute set of derivations, relative to the provided
  *installable*, to JSON:

  ```console
  # nix eval-drvs github:NixOS/nix -A checks.x86_64-linux
  ```

* Evaluate attribute paths prior to evaluating an attribute set of derivations,
  relative to the provided *installable*, to JSON:

  ```console
  # nix eval-drvs github:NixOS/nixpkgs#legacyPackages.x86_64-linux -A python3Packages -C lib python3
  ```

# Description

This command evaluates an attribute set of Nix derivations and prints the
result on standard output as JSON.

Attribute paths provided by the `-C`/`--common-attr-paths` option are
evaluated in order, sequentially, relative to the root of *installable*,
without generating JSON output. This functionality allows the user to force
the evaluation of thunks in the main process before recursing into
*installable*, or the value at the attribute path given by `-A`/`--attr-path`.
As an example, if one were to use `nix eval-drvs` on Nixpkgs, one could prefix
evaluation with items such as

- `lib` and its children to ensure the evaluation of the Nixpkgs library
- `hello` to force the evaluation of the top-level, `stdenv`, and `mkDerivation`
- `python3Packages` to force the evaluation of attribute names in the Python package set

# Output format

`nix eval-drvs` produces JSON output.

)""
