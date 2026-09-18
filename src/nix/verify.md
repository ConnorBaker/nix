R""(

# Examples

* Verify the entire Nix store:

  ```console
  # nix store verify --all
  ```

* Check whether each path in the closure of Firefox has at least 2
  signatures:

  ```console
  # nix store verify --recursive --sigs-needed 2 --no-contents $(type -p firefox)
  ```

* Verify a store path in the binary cache `https://cache.nixos.org/`:

  ```console
  # nix store verify --store https://cache.nixos.org/ \
      /nix/store/10l19qifk7hjjq47px8m2prqk1gv4isy-hello-2.10
  ```

# Description

This command verifies the integrity of the store paths [*installables*](./nix.md#installables),
or, if `--all` is given, the entire Nix store. For each path, it
checks that

* its contents match the [object hash](@docroot@/store/file-system-object/content-address.md#git)
  recorded for it (or, for a path described by an older binary cache
  or peer that asserts only a [Nix Archive][] hash, that NAR hash); and

* it is *trusted*, that is, it is signed by at least one trusted
  signing key, is content-addressed, or is built locally ("ultimately
  trusted"). A version-1 signature (over the NAR hash) is checked
  against the NAR hash the path's description asserts or, for a
  locally held path whose database row holds the object hash alone,
  against one walk of the path, made at most once per path and only
  for a signature by a trusted key; so the version-1 signatures a
  `--substituter` from an older cache offers are usable, at the cost
  of that walk.

# Exit status

The exit status of this command is the sum of the following values:

* **1** if any path is corrupted (i.e. its contents don't match the
  recorded hash).

* **2** if any path is untrusted.

* **4** if any path couldn't be verified for any other reason (such as
  an I/O error).

[Nix Archive]: @docroot@/store/file-system-object/content-address.md#serial-nix-archive

)""
