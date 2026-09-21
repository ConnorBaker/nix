R""(

# Examples

* Migrate a store written by an older version of Nix into the object store:

  ```console
  nix store optimise
  ```

# Description

The Nix store deduplicates itself as paths are added: each store path's
regular files are entered into the store's object store,
`/nix/store/.objects/`, which keeps each distinct file once as a git
blob under SHA-256, and the path's files are hard links to those blobs'
files. No setting turns this on; the `auto-optimise-store` setting of
older versions is accepted and ignored, with a warning when it is set
to `true`.

This command completes that state for a store an older version of Nix
wrote: it walks every valid store path, enters any file that is not yet
the object store's and replaces it by a hard link, and removes the links
directory older versions kept, `/nix/store/.links/`, whose entries were
hard links to files store paths also link, or dead ones, so removing it
frees whatever dead entries an older collector left and loses nothing
(the garbage collector removes it too). On a store
this version has written all along it has nothing to link and says so
(`0.0 KiB freed by hard-linking 0 files; 0 files entered as new
objects; 0 paths migrated to the object hash`); running it again
changes nothing.

Executable and non-executable files with the same contents are one blob
but two files, since a hard link cannot carry two modes.

)""
