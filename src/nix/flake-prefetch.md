R""(

# Examples

* Download a tarball and unpack it:

  ```console
  # nix flake prefetch https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.10.5.tar.xz --out-link ./result
  Downloaded 'https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.10.5.tar.xz?treeHash=sha256-0bQZ/PkmPEdd0UfrTebgKhSiyamuXLQ%2BUpQtfdWmX/k%3D'
  to '/nix/store/w4dwngn0cp5rfgk95v2j1ycwglcpmvll-source' (hash
  'sha256-0bQZ/PkmPEdd0UfrTebgKhSiyamuXLQ+UpQtfdWmX/k=').

  # cat ./result/README
  Linux kernel
  …
  ```

* Download the `dwarffs` flake (looked up in the flake registry):

  ```console
  # nix flake prefetch dwarffs --json
  {"hash":"sha256-IjFBbAAu9cCMmlpnVX1Vv73aACKrZ0ZTt9JFQRdNLvQ="
  ,"storePath":"/nix/store/rma6brjxd5cwsbxhqinhm39xpglq9260-source"}
  ```

# Description

This command downloads the source tree denoted by flake reference
*flake-url*. Note that this does not need to be a flake (i.e. it does
not have to contain a `flake.nix` file).

)""
