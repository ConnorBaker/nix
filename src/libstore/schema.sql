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
-- (blob, mode, ingestion). Each ingestion owns its candidate until its
-- registration makes that candidate visible. A failed concurrent ingestion
-- must not replace a registered holder's record.
--
-- The holder's store path is named through `Ingestions`, not by a foreign
-- key into `ValidPaths`. Rows stay private in the hold until the path is
-- complete. Ordinary registration inserts them with validity in one transaction.
-- `endIngestion` can persist a completed builder output before bulk validity
-- registration, allowing its hold to begin the next output. It publishes an
-- already-valid repair/optimisation path itself. Both publication routes reserve
-- SQLite's writer and reconcile candidates before exposing the complete projection.
--
-- Completed builder projections, and failed or legacy rows, must survive a
-- collection while their hold remains (08 K16). The ingestion records that hold's
-- name. The sweep removes dead holds before judging rows and keeps rows whose
-- hold still exists. Private pending rows need no database cleanup.
--
-- What that costs, and where it is paid: a row whose ingestion has no path, or
-- whose path is no longer valid (the path was collected), is *stale*.  Two
-- rules apply: holder lookup requires a valid named path and conditionally
-- forgets an observed candidate whose file is missing or invalid; the sweep
-- removes stale ingestions under the GC lock once their holds are gone. Their
-- regular and auxiliary projections are removed by the ownership foreign keys.
create table if not exists Ingestions (
    id   integer primary key autoincrement not null,
    path text, -- completed holder path; null while staged before naming
    hold text  -- the name of its hold directory under .objects/tmp while it is in flight
);

create table if not exists Blobs (
    id        blob    not null, -- the blob's identifier, its 32 bytes
    exec      integer not null, -- the mode: 0 or 1
    ino       integer not null, -- the holder's inode
    dev       integer not null, -- and its device
    ingestion integer not null references Ingestions(id) on delete cascade,
    name      text    not null, -- the holder's name under that path ("" for a plain-file root)
    size      integer not null,
    primary key (id, exec, ingestion)
) without rowid;

create index if not exists IndexBlobsIngestion on Blobs(ingestion);

create index if not exists IndexIngestionsPath on Ingestions(path, hold);

-- Derived directory bodies and symlink targets, owned by the same completed
-- ingestion as its regular-holder projection. A body is the concatenation of
-- contiguous zero-based chunks; the empty value has one empty chunk. Reads
-- acquire one incarnation in one statement before any destination callback.
-- A reused value is copied into the receiving projection, so removing its
-- donor never removes the receiver's value. Unlike a physical holder, bytes
-- are independently verified and eligible before ValidPaths publication.
create table if not exists AuxValues (
    kind      integer not null check(kind in (16384, 40960)), -- Directory or Symlink
    id        blob not null check(typeof(id) = 'blob' and length(id) = 32),
    ingestion integer not null references Ingestions(id) on delete cascade,
    chunk     integer not null check(chunk >= 0),
    body      blob not null,
    primary key(kind, id, ingestion, chunk)
) without rowid;

create index if not exists IndexAuxValuesIngestion on AuxValues(ingestion);
