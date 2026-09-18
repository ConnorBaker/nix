# Name

`nix-store --optimise` - migrate an older store into the object store

## Synopsis

`nix-store` `--optimise`

## Description

The Nix store deduplicates itself as paths are added: each store path's
regular files are entered into the store's object store,
`/nix/store/.objects/`, which keeps each distinct file once as a git
blob under SHA-256 (see [Git content addressing][Git]), and the path's
files are hard links to those blobs' files. No setting turns this on;
the `auto-optimise-store` setting of older versions is accepted and
ignored, with a warning when it is set to `true`.

The operation `--optimise` completes that state for a store an older
version of Nix wrote. It walks every valid store path, enters any
regular file that is not yet the object store's and replaces it by a
hard link, and removes the links directory older versions kept,
`/nix/store/.links/`, whose entries were hard links to files store
paths also link, or dead ones, so removing it frees whatever dead
entries an older collector left and loses nothing (the garbage
collector removes it too). A path an older version registered is
first migrated to its object hash by the walk `nix store migrate`
makes, which checks the NAR hash the row asserts; a path whose files
no longer match it, or are missing, is reported and not entered (see
`nix store migrate`). On a store this version has written
all along it has nothing to link and says so (`0.0 KiB freed by
hard-linking 0 files; 0 files entered as new objects; 0 paths migrated
to the object hash`); running it again changes nothing.

Executable and non-executable files with the same contents are one
blob but two files, since a hard link cannot carry two modes; symlinks
and directories are not hard-linked. A file the store cannot link is
left as it is: a blob at the filesystem's hard-link limit, a file under
`.app/Contents` on macOS, and every file on a filesystem that cannot
hard-link.

After completion, a report is printed on standard error: the bytes
freed and the number of files replaced by hard links to a blob the
store already held, the number of files entered as new blobs, and the
number of database rows written by an older version that were given
their object hash by the walk (what `nix store migrate` does; the
walk that enters a path is the walk the migration needs).

Use `-vv` or `-vvv` to get some progress indication (one `optimising
path` line per store path).

{{#include ./opt-common.md}}

{{#include ../opt-common.md}}

{{#include ../env-common.md}}

## Example

On a store an older Nix wrote (illustrative figures; the shape of the line is the command's):

```console
$ nix-store --optimise
516.7 MiB freed by hard-linking 54143 files; 187012 files entered as new objects; 6301 paths migrated to the object hash
```

On a store this Nix has written all along:

```console
$ nix-store --optimise
0.0 KiB freed by hard-linking 0 files; 0 files entered as new objects; 0 paths migrated to the object hash
```

[Git]: @docroot@/store/file-system-object/content-address.md#git
