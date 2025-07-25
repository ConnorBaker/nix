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

# Description

This command evaluates an attribute set of Nix derivations and prints the
result on standard output as JSON.

# Output format

`nix eval-drvs` produces JSON output.

)""
