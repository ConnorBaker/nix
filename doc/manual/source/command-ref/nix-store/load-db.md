# Name

`nix-store --load-db` - import Nix database

# Synopsis

`nix-store` `--load-db`

# Description

The operation `--load-db` reads a dump of the Nix database created by
`--dump-db` from standard input and loads it into the Nix database.

The records of a dump carry each path's object hash (the record format
is that of [`--register-validity`](./register-validity.md)). A dump
written by an older Nix, whose records carry NAR hashes, is loaded as
well: each path is walked once to compute its object hash and to check
the NAR hash and size the record asserts; a record whose path does not
match is reported and skipped, the rest are registered, and the exit
status is 1 if any record was skipped. An older Nix cannot load a dump
written by this version.

{{#include ./opt-common.md}}

{{#include ../opt-common.md}}

{{#include ../env-common.md}}
