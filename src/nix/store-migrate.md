R""(

# Examples

* Migrate the Nix store:

  ```console
  # nix store migrate
  ```

# Description

This command fills in the *object hash* of every store path that an
older version of Nix registered. Such a path's database row holds the
hash of its NAR serialisation; this Nix identifies a store path by the
hash of its content as a git tree under SHA-256, and computes it for
an old row with one walk of the path, after which the row is current.

Running this command is not needed for correctness: a path registered
by an older Nix is migrated the first time it is queried. It makes
that first query cheap, and with it the first `nix-collect-garbage`
after upgrading, which queries every path. Each path is migrated in
its own transaction, so the command is safe to interrupt and to run
again; a path already carrying its object hash is left alone.

The walk that computes a path's object hash also checks the NAR hash
its row asserts, as `nix-store --load-db` checks a registration. A
path whose files no longer hash to it -- modified since the older Nix
wrote them -- is not migrated: the command reports it as modified,
names both hashes and exits with status 1; `nix-store --verify
--check-contents --repair` restores the path, after which the next
query or run migrates its row. Until then a query of the path answers
from the row as it stands, and `nix store verify` and `nix-store
--verify-path` report the modification as before.

A path whose row is valid but whose files are gone -- removed by hand,
or lost before an older Nix's `nix-store --verify` ran -- is not
migrated either: the command reports it and names `nix-store
--verify`, which removes such a row. A query of the path still answers
from the row, as before.

Until every path has been migrated, garbage collection reclaims no
space from the object store under `/nix/store/.objects/`, since a
path not yet migrated is a root it cannot see. If any path could not
be migrated (for instance because its files are unreadable), the
command reports it and exits with status 1. A path collected by the
garbage collector while the command runs is skipped and counted, not
reported as a failure.

)""
