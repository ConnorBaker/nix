create table if not exists ValidPaths (
    id               integer primary key autoincrement not null,
    path             text unique not null,
    hash             text not null, -- object hash, "git:sha256:<base16>"; "sha256:<base16>" in a row not yet migrated
    registrationTime integer not null,
    deriver          text,
    narSize          integer,
    ultimate         integer, -- null implies "false"
    sigs             text, -- space-separated
    ca               text -- if not null, an assertion that the path is content-addressed; see ValidPathInfo
);

create table if not exists Refs (
    referrer  integer not null,
    reference integer not null,
    primary key (referrer, reference),
    foreign key (referrer) references ValidPaths(id) on delete cascade,
    foreign key (reference) references ValidPaths(id) on delete restrict
);

create index if not exists IndexReference on Refs(reference);

-- Paths can refer to themselves, causing a tuple (N, N) in the Refs
-- table.  This causes a deletion of the corresponding row in
-- ValidPaths to cause a foreign key constraint violation (due to `on
-- delete restrict' on the `reference' column).  Therefore, explicitly
-- get rid of self-references.
create trigger if not exists DeleteSelfRefs before delete on ValidPaths
  begin
    delete from Refs where referrer = old.id and reference = old.id;
  end;

create table if not exists DerivationOutputs (
    drv  integer not null,
    id   text not null, -- symbolic output id, usually "out"
    path text not null,
    primary key (drv, id),
    foreign key (drv) references ValidPaths(id) on delete cascade
);

create index if not exists IndexDerivationOutputs on DerivationOutputs(path);

-- The blob index (01 section 9.10): a regular-file blob's canonical file is a
-- store path's own file -- a *holder* -- and this is where it is.  One row per
-- (blob, mode).
--
-- The holder's store path is named through `Ingestions`, not by a foreign
-- key into `ValidPaths`. A large ingestion writes batches during the copy,
-- creating its `Ingestions` row lazily at the first flush with a null path.
-- Registration assigns that path and publishes the remaining rows. Small
-- ingestions publish all their rows in the path-registration transaction,
-- through its connection, without a background writer or separate commit.
--
-- An ingestion still in flight has no path yet, and its rows must survive a
-- collection that runs beside it exactly as its objects do (08 K16): the row
-- carries its hold's name, and the sweep -- which has already removed the holds
-- of dead ingestions before it judges anything -- keeps the rows of an
-- ingestion whose hold is still there.
--
-- What that costs, and where it is paid: a row whose ingestion has no path, or
-- whose path is no longer valid (the path was collected), is *stale*.  Two
-- things judge it: `GitObjectStore::link` finds no file at the holder's name
-- and forgets the row, writing the blob afresh; and the sweep removes every row
-- whose ingestion names no valid path, under the GC lock, as it removes a tree
-- no live root reaches.  This is the same discipline the trees and the symlink
-- blobs are already collected by, and it replaces the cascade a foreign key
-- would have given.
create table if not exists Ingestions (
    id   integer primary key autoincrement not null,
    path text, -- the store path of the ingestion's holders; null until it is registered
    hold text  -- the name of its hold directory under .objects/tmp while it is in flight
);

create table if not exists Blobs (
    id        blob    not null, -- the blob's identifier, its 32 bytes
    exec      integer not null, -- the mode: 0 or 1
    ino       integer not null, -- the holder's inode
    dev       integer not null, -- and its device
    ingestion integer not null, -- Ingestions.id: which ingestion's path holds it
    name      text    not null, -- the holder's name under that path ("" for a plain-file root)
    size      integer not null,
    primary key (id, exec)
);

create index if not exists IndexBlobsIngestion on Blobs(ingestion);
