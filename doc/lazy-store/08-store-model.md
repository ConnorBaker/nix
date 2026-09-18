# The local store's concurrency and durability model, and the API semantics it rests on

A reference for a maintainer about to touch `src/libstore`. Section 1 states the facts the local store's code keeps, one per sentence, each with its `file:line`; line numbers were taken when the fact was written and drift, so a locator names the function beside it where the line may have moved; where the lazy-store branch's code depends on a fact, the dependency is named beside it. Section 2 is the checklist of API semantics to answer at every call site of the kinds the branch uses. Neither section argues; the arguments are in `01-specification.md` and `04-derivation.md`, and the tests that pin the defects behind these facts are the table of `07-defect-ledger.md`.

## 1. The model

### 1.1 Locks

**The big lock** (`<state>/db/big-lock`).
- A writable store opens it and holds it shared for its life (`local-store.cc:243-261`).
- The lock is taken exclusive only for a schema upgrade, releasing the shared lock first to avoid a promotion deadlock (`local-store.cc:277-283`, `345`, `356`).
- A read-only store takes no lock at all (`local-store.cc:243`, `258`).
- Branch: the schema is 11; a schema-10 store is bumped on first writable open and runs no statement for it (`local-store.cc:266-273`, `287-341`, `local-store.hh:29-33`); an older Nix then refuses the store. A read-only open of a schema-10 store is allowed (`local-store.cc:267-273`).

**The database lock** (`_state`, a `Sync<State>`).
- Every SQLite statement runs with `_state` locked; `retrySQLite` retries `SQLITE_BUSY` (`local-store.cc:797-798`, `1041`, `1053-1056`).
- A transaction (`SQLiteTxn`) is opened and committed under one lock scope (`local-store.cc:1187-1190`, `1727-1730`).
- Nothing that walks the filesystem may run inside a transaction or under the lock.
- Branch: `migratedPathInfo` walks the path outside the lock and re-reads the row under the lock before writing, so a row migrated or deleted meanwhile is respected (`walkOldRow`, `local-store.cc:840-888`; `migratedPathInfo`, `890-925`; the re-read in `recordObjectHash` at `939`). `forEachValidObjectHash` runs under the lock and therefore counts unmigrated rows instead of migrating them (`local-store.cc:1065-1082`). `addSignatures` migrates first, outside its transaction (`local-store.cc:2044-2049`). `queryPathInfoInternal` never migrates (`local-store.hh:590-596`), nor does `queryPathInfoUnmigrated`, the same read under the lock it takes, which the collector uses for its dead set.

**The GC lock** (`<state>/gc.lock`).
- `collectGarbage` holds it exclusive for its whole run (`gc.cc:394-395`).
- `addTempRoot` tries it shared and non-blocking (`gc.cc:105`); when that fails the collector is running and the root is sent over the collector's socket, and the sender waits for the acknowledgement (`gc.cc:107-153`).
- The collector's server thread records each socket root in `tempRoots` and, while the path is being deleted (`pending`), makes the sender wait until the deletion is done (`gc.cc:470-500`, `481-492`; `pending` set at `675` and `738`, reset at `589` and `742`).
- The collector re-checks `tempRoots` immediately before each deletion (`gc.cc:719-744`).
- `verifyStore` takes the GC lock shared and blocking, so it never runs beside a collection (`local-store.cc:1755`).
- Branch: the object-store sweep runs inside the exclusive lock, after the deletions (`gc.cc:864-874`). No branch code takes a temp root on behalf of a query the collector itself makes — the migration takes none (`local-store.cc:895-901`), because the collector reads visited paths' rows — unmigrated, as the database has them (`queryPathInfoUnmigrated`; `topoSortPathsBy`, `gc.cc`), so a dead schema-10 row is deleted without the walk — and the live closure's infos through `computeFSClosure`, and a root registered then would keep a dead path.

**Path locks** (`<realPath>.lock`).
- One path's write is guarded by `PathLocks` on its real path, except from a build hook whose parent holds the lock (`local-store.cc:1292-1304`, `1627`).
- The validity check is repeated under the path lock, because another process may have registered the path meanwhile (`local-store.cc:1305`, `1630`).

**Temporary directories in the store** (`tmp-*`).
- `createTempDirInStore` makes the directory, opens it, and takes an exclusive lock on the descriptor, retrying until the directory both exists and is locked (`createTempDirInStore`, `local-store.cc:1703`).
- The collector deletes a `tmp-*` entry only when it can take that lock itself (`gc.cc:551-558`).
- Branch: `addToStoreFromDump` restores through the object store inside such a directory and then moves it into place (`local-store.cc:1581-1590`, `1660`); the blobs it entered stay linked across the move (same filesystem), the trees it wrote are not protected by the lock (see 1.5, X1). `LocalStore::materialise` (`optimise-store.cc`) does the same, step for step.

### 1.2 Temporary roots

- A process's temp roots live in `<state>/temproots/<pid>`, a file the process holds write-locked (`gc.cc:54-82`, `71`).
- A root is a path name appended to that file, or, during a collection, a line sent to the collector's socket (`gc.cc:155-158`, `132-138`).
- The collector reads every temproots file after taking the GC lock; a file whose lock it can acquire belongs to a dead process and is removed (`gc.cc:163-215`, `527-534`).
- The file is unlinked when the store object is destroyed (`local-store.cc:471-476`); a command that has exited roots nothing.
- A read-only store creates no temp roots (`gc.cc:86-90`).
- A temp root protects a store path; it protects no object-store object.
- Branch: `WriteBuffer::addText` roots the pending path and each non-pending reference at enqueue (`write-buffer.cc:49-57`); `fetchToStore2` roots before `isValidPath` on a cache hit (`fetch-to-store.cc:292-295`); `optimiseStore` roots each path before walking it (`optimise-store.cc:454`); `nix store migrate` roots before its walk (`store-migrate.cc:60`); the daemon roots before the shim's walk for an old client (`daemon.cc:321-331`).

### 1.3 Between a write and a registration

- A path is written into its real path or into a temp directory, verified, optionally synced, and registered in one transaction (`addToStore`, `local-store.cc:1265`; `addToStoreFromDump`, `1483`; `registerValidPaths`, `1176`).
- Until `registerValidPaths` commits, the path is invalid; the collector deletes an invalid entry unless a temp root names it (`gc.cc:678`, `819-822`); both ingestions take the root first (`local-store.cc:1290`, `1618`).
- A verification failure after the restore leaves the written files in place as an invalid path, which a later collection removes (`addToStore`, `local-store.cc:1265`: the checks between the restore and `registerValidPath`); two of the checks remove the files themselves — the refusal of a SHA-1 git address (`1377-1378`) and the deferred signature check (`1436-1441`).
- A crash before the commit leaves an invalid path and, on the branch, blob files with two links and tree files nothing reaches; the collection removes the path, then the blobs (link count 1) and the trees (unreachable).
- The database row is durable by SQLite's `synchronous = normal` under `fsync-metadata` (`local-store.cc:557`); the files are durable only under `fsync-store-paths` (1.4).
- Branch: `restoreThroughObjects` writes blobs (`place`; `putBlob` for a symlink's target) and trees (`putTree`) while restoring, before any registration (`optimise-store.cc`, `LinkingVisitor`); those objects are visible to a concurrent sweep (1.5). The ingestion's own `autoGC()` runs before the restore, not in this window, and on the in-memory route after the validity check, so an add of a valid path runs none (`LocalStoreAutoGCTest`).

### 1.4 Durability points

- Under `fsync-store-paths`, every file of the path is synced and then its parent directory, before registration (`local-store.cc:1448-1450`, `1683-1685`).
- The restore sink starts an asynchronous write-back per file when `startFsync` is set; it waits for none (`fs-sink.cc:264-269`).
- Under `sync-before-registering`, `sync()` runs before every registration (`local-store.cc:1183-1184`).
- Without either setting, nothing makes the files durable before the row.
- Branch: object files are fsynced under the same `fsync-store-paths` flag — the file before it is linked, the directory after (`git-object-store.cc:245-246`, `259-260`; the flag from `local-store.cc:133`). The mode and mtime canonicalisation happens before the file's fsync (`writeObjectFile`, `git-object-store.cc`), so that it is covered by it.

### 1.5 The object store's invariants

The object store is `<realStoreDir>/.objects/{blobs,blobs-x,trees,tmp}` (`git-object-store.hh:24-27`), made when a writable store opens (`local-store.cc:150`).

Kept by the code:
- K1. A blob file is created either as a second link to a store path's file already written and canonicalised (`place`'s miss branch, `git-object-store.cc`, `linkDirEntryToPath`) or from bytes through a temporary file that is linked into place (`writeObjectFile`); no blob file is ever written in place.
- K2. A store path's file is linked to a blob file only after the blob's size was checked, and its bytes too under repair; a failing file is removed from the object store first (`blobFileOrRemove`, `git-object-store.cc:92-108`; `link` at `110-134`).
- K3. A concurrent entry of the same identifier is resolved by trying twice, the loser linking to the winner's file (`place`, `git-object-store.cc:174-209`).
- K4. A blob file with one link is removed by the sweep unless a reachable tree names it (`git-object-store.cc:414-436`).
- K5. Trees are reachable only from the column: the sweep's roots are the object hashes of the valid paths (`git-object-store.cc:348-353`; `forEachValidObjectHash`); while any row is unmigrated, no tree and no blob is removed (`358`).
- K6. A tree body that does not hash to its name aborts the removal phase (`git-object-store.cc:381-404`).
- K7. The collector's store-directory walk passes over `.objects` and `.links` (`gc.cc:807-822`).
- K8. Every regular file of a store path that the store could link has a link count of at least two; the on-disk walk trusts an inode as a blob's only when the file has two links and the inode is in the table (`optimise-store.cc:195-207`).
- K9. The synthetic root tree of an executable or symlink root is written so that its blob is reachable (`optimise-store.cc:422`, `438`; `merkle-hash.cc:87-98`).
- K10. Trees are written bottom-up, the root last (`ObjectStoreVisitor::directory`; `putRootTree` after the walk), and the sweep removes every unreachable tree in one loop, so a sweep that took a root took every unreachable subtree of it; hence a live root whose object is missing keeps nothing beneath it and does not stop the sweep (`SweepStats::unentered`; `01` §9.10 law 5), while a corrupt tree body does (K6).
- K11. Whether the store places a regular file already on disk — gives it a blob and links it — is decided in one place, `placementOf` (`optimise-store.cc`): a writable file and a file this system does not link (`.app/Contents` on macOS, every file on Windows) are left as written; the on-disk walk skips them and the verifier expects no blob for them. The restore route decides before the file exists and asks `linkableOnThisSystem` directly (its files are never writable, having been canonicalised): two sites, one rule. A file `place` could not enter for `ENOSPC` is placed by neither and expected by the verifier (law 3's fourth exception, unverifiable without a full directory index).
- K12. The sweep and the inode table make the four directories before iterating them (`createDirectories`), so a directory removed after the open does not abort a collection (`LocalStoreObjectsTest.sweepMakesAMissingObjectDirectory`).
- K13. On the local overlay store the merged store directory shows the lower store's `.objects` and `.links` as well, and a write through it lands in the upper layer (an unlink of a lower file is a whiteout, a repair a copy-up); so the sweep, the `.links` removal and the verifier's object checks are not run there (`LocalStore::ownsObjectStore`, false for `LocalOverlayStore`; by reading, Linux's). The upper layer's own objects are then not reclaimed; a sweep over the upper layer alone is owed, for the Linux run.
- K14. A schema-10 row's migration checks the NAR hash the row asserts on a tee of the walk that computes the object hash, and writes the column only when they agree (`walkOldRow`); a row whose files are modified or missing is answered as the database has it, and the verifiers report it (`contentMismatch`, `verifyStore`, `nix-store --verify`). A temporary under `.objects/tmp` carries the second of its making in its name and ages by it, since a temporary link's inode is a blob's (`tempPath`).
- K15. A path materialised from the store's own objects (`01` §9.10, the second route; `MaterialisingVisitor`, `optimise-store.cc`) is made only from a tree body that hashes to its name (`GitObjectStore::readVerifiedTree`, checked on every read, a corrupt body refused and left for `--verify --check-contents --repair`), from blob files linked as K2 links them (the size unchecked, the route having no other measure of the file; the bytes re-hashed under repair), and from a symlink's blob checked against its identifier; a directory whose objects the store lacks or holds corrupt is removed as far as it was made and read from the source through the one route's visitor. The objects such a copy reads are reachable from the valid paths that entered them, so the sweep keeps them (K5); one taken by X1's window falls back. The route is taken under one predicate, `LocalStore::materialisesFromObjects` — a store that owns its object store, not Windows — asked in `fetchToStore2` alone; the daemon, the overlay store and Windows keep the one route.

Not kept, stated:
- X1. The trees an ingestion writes before its path is registered — and the trees `nix-store --optimise` writes when it enters a path, since it takes no GC lock (`optimise-store.cc`; `nix store migrate` writes no object: `walkOldRow` hashes and records the column alone) — are not protected from a *concurrent* sweep (another process's collection, the builder's asynchronous `autoGC(false)`): they are unreachable from the column and are removed, and so are the path's symlink-target blobs, in that same sweep; its regular-file blobs are held by its own links. The path is then registered with an object hash naming a tree the store lacks; what follows is `01` §9.10 law 4's second false-by-design case and the first open item of `01` section 13. The ingestion's own synchronous `autoGC()` cannot do this: it runs before the restore (`LocalStoreAutoGCTest`).
- X2. Law 4's concurrent form. `verifyStore --check-contents` checks that every blob and tree file hashes to its name, that every valid path's object hash matches its files, and that the objects a path reaches are present, by the same walk that computes the hash (`verifyObjects`, `VerifyVisitor`), expecting no blob for a file the store does not place (K11) and saying "entered" under `--repair` only when the re-check finds nothing missing. Law 4 is checked per path; what remains unchecked is its concurrent form (`05-validation.md` section 7).
- X3. A specific delete reclaims no object. The sweep runs on whole-store collections alone (`gc.cc`, `gcDeleteDead`); `nix store delete` sweeps nothing, so a deleted path's objects stay until the next `nix-store --gc`, and its freed-bytes figure counts them as freed (`deletePath`'s two-link credit) — the collector says so. The legacy `.links` directory is removed at every whole-store collection as well (`removeLegacyLinks`).

### 1.6 The order in which the sinks fire

The NAR parser (`archive.cc:211-341`):
- For a regular file: `createRegularFile(path, f)`, and inside `f`: `isExecutable()` if the entry is executable, then `preallocateContents(size)`, then the bytes (`archive.cc:242-261`, `181-201`).
- For a directory: `createDirectory(path, cb)`, the children delivered inside `cb`, in sorted order (`archive.cc:263-326`).
- For a symlink: `createSymlink(path, target)` (`archive.cc:328-337`).
- A colliding name under `use-case-hack` is renamed with the suffix before it is delivered (`archive.cc:298-317`); a name already carrying the suffix is refused on every platform (`292-294`).

`RestoreSink` (`fs-sink.cc`):
- `createSubdirectory` makes the directory, opens it, hands the callback a fresh `RestoreSink` whose `dstPath` is the directory and whose path is `/`, and runs `directoryDone` when the callback returns (`fs-sink.cc:119-155`).
- `createRegularFile` is `beginRegularFile`, the function, `finish` (`fs-sink.cc:267`); `finish` flushes and runs `regularFileCreated` once; the destructor flushes again and starts the write-back (`RegularFileWriter`, `fs-sink.cc:238-266`).
- The generic `createDirectory(path, cb)` default of the base hands the callback the same sink and the full path (`fs-sink.hh:62-64`); `RestoreSink` hands it a sink of the directory's own under `/` (`fs-sink.cc:119-155`). The two conventions differ; `copyRecursive`'s `Copy` visitor keeps a stack of what the callback handed back, seeded with the caller's sink and destination (`tree-traversal.cc`), so it serves both.

The one traversal and its visitors: `traverse` and `NodeVisitor` (`src/libutil/include/nix/util/tree-traversal.hh`), `HashingVisitor` and the push adapter `ObjectHashSink` (`src/libutil/include/nix/util/object-hash-sink.hh`) carry their own contracts — the visit order is `dumpPath`'s, a regular file is delivered in the sink protocol's order, a directory is a scope in which its children are visited, `known` is consulted before any read by the pull driver and never by the push driver, and every name handed to a visitor is the tree's own (`unhackName`). The store's visitors are below.

The store's visitors (`optimise-store.cc`):
- `ObjectStoreVisitor` (`:178`) writes each symlink's target as a blob and each directory's body as a tree after the hasher returns them; regular files are the two drivers' business.
- `EnterVisitor` (`:207`), the on-disk walk (`enterIntoObjects`, `traverse` over `makeFSSourceAccessor`): `known` answers a regular file whose inode is a blob's and which has two links; `regular` asks `placementOf` (`:58`), then makes the parent writable (`WritableDirectory`, `:76`) and places the file (`placeRegularFile`, `:133`, `GitObjectStore::place`); a writable file is left alone as suspicious.
- `LinkingVisitor` (`:339`), the restore route (`restoreThroughObjects`, `parseDump` → `ObjectHashSink` → this): its `open` stack holds the `RestoreSink` each directory's callback handed back, seeded with the caller's, so a node is created on the innermost sink under `/name` (`at`); `directory` opens `createSubdirectory`'s scope and visits the children inside it, so `directoryDone` (canonicalisation) runs when the scope closes, before the tree is written; `symlink` creates the link then hashes.  A regular file goes to the hasher as it arrives and is held in memory up to `restoreBufferLimit` (`:329`); when the hasher returns the entry, the visitor decides with it in hand: a blob the store holds is linked (`objects.link`, one `linkat`), nothing written, so `regularFileCreated` does not fire; a miss is written from memory and placed.  A file announced or grown above the limit is opened at once (`BufferThenStreamFile`), streamed, finished and placed.
- `MaterialisingVisitor` (`optimise-store.cc`), the copy from the store's own objects (`materialiseThroughObjects`, `traverse` over the source accessor with this visitor, a `LinkingVisitor` beneath): `known`, asked by the pull driver for the root and every directory, takes the node's entry from the naming's memo (`LocalStore::TreeNamer`, `MemoVisitor::lookup`) and makes the subtree from the store's objects — `mkdirat`, `link` per regular file, `symlinkat` from the target's blob, the hooks' `directoryDone` and `symlinkCreated` on each, in the NAR's order under the case hack (`CaseHackNames`) — returning the entry and the NAR size it summed; a miss (no row, no body, a corrupt body, a blob the store lacks) removes what was made and returns nullopt, so the driver lists the directory and hands its children to `LinkingVisitor`'s actions as the one route would. `directory` keeps the on-disk name of each child on `diskNames` around its visit, since the driver hands the accessor's name and `at` needs the hacked one. `regularFileCreated` fires for the files the fallback writes and for none linked.
- `regularFileCreated` fires only for files written, never for files linked (`local-store.hh`, the counting hooks of `LocalStoreObjectsTest`); with `GitObjectStore::link` forced false the four counting assertions fail (`05-validation.md` section 4).

## 2. The semantics checklist

At every call of one of these kinds, the caller writes the assumption as a comment and cites the line that guarantees it. The questions to answer:

**`flock` / `FdLock`.** Who holds which mode now? If the collector holds it exclusive, does my shared attempt fall back to the socket (`gc.cc:107-153`), and do I want a root the running collection honours? Is the lock on this open file description, or on another open of the same file? Does the process that must hold it outlive the operation it protects?

**`linkat` / `link`.** Which errno do I tolerate — `EEXIST`, `EMLINK`, `ENOENT`, `ENOSPC` — and what happens to the file I was about to link in each case? Does the target directory need write permission, and who restores its canonical mode on every exit path, exceptions included? Does the link count I read include the link I am about to make?

**`renameat` over an existing name.** Is the target directory writable? Is the inode I replace still linked elsewhere, or do its bytes vanish? Is the rename within one filesystem (else `EXDEV`, handled at `local-store.cc:1660-1662`, `movePath`)? Is the temporary I rename from removed on every failure?

**`fstatat` / `lstat` / `readdir`.** Do I key a table by `d_ino` or `st_ino`, and does the reader of the table use the same one? Is `st_size` a cheap negative or a proof of identity (it is the former)? Is `st_nlink >= 2` the fact I need? Is the directory being modified while I iterate it? Which of mtime and ctime moves when I link, unlink or chmod?

**`fsync`.** File before link, directory after link; is any metadata changed after the file's fsync? One fsync per file, or `recursiveSync` once? Under which setting?

**`std::filesystem::path::operator/`.** Can the right operand be empty (`"x"/""` is `"x/"`) or absolute (it replaces the left)?

**Containers.** Do I hold a reference or iterator into the element I erase? Do I insert into a map while iterating it?

**`enable_shared_from_this`.** Am I in a constructor, or in a function that may run on a stack object?

**Types as invariants.** Does the token have a user-provided move constructor? Are the members meant to be unreachable `private`, not `protected`? Does the type's constructor do what the type's name promises?

**Destructors and exits.** Does this exit path run destructors — `exec` does not, `_exit` does not? Can the destructor's work fail, and who reports the failure? Is the environment the destructor needs still in place?

**Meson.** Is `--suite` combined with names on one line (the intersection runs)? After a dependency change, does `meson configure build | grep pkg_config_path` show the new path?

**libgit2.** Which layer enforces the rule I rely on — the tree builder (`tree.c:57`), the object writer (`odb.c:1613`), the parser (`tree.c:394`), or the attribute lookup with its flags (`include/git2/attr.h`; `GIT_ATTR_CHECK_INDEX_ONLY` reads the index, where an untracked `.gitattributes` cannot appear, while git itself reads a working directory's attribute files tracked or not; `GIT_ATTR_CHECK_NO_SYSTEM` excludes the system file)? Is `GIT_OPT_ENABLE_STRICT_HASH_VERIFICATION` on, and if not, who verifies what I read?

**Git object formats.** Are the entries ordered as if directory names ended in `/` (`merkle-hash.hh:24-27`)? Does the root carry a mode (it does not)? Does my memo row hold the entry hash or the object hash of a synthetic root, and which does every reader expect?

**Protocols.** Which peer versions and features can be on the other end of this call, in both directions? What does the info carry when the peer is old — `objectHash` absent, so never dereferenced unguarded? Which side pays the shim's walk, and how often?

**Signatures.** Which fingerprint version does each peer verify? Is the NAR hash a version-1 signature needs available where the signature is checked? Does `require-sigs` on the receiver see the info as sent, before anything is stored (`daemon.cc:555-576`, `local-store.cc:1281`)?

**The store directory.** Before adding, renaming or removing anything under it: read the collector's walk (`gc.cc:807-823`), `verifyAllValidPaths` (`local-store.cc:1940`), and every `rmdir`, `ls` and emptiness assertion in `tests/functional`.

**The base class.** Before deriving from or composing over a sink or a store: list every virtual the base calls on child objects (`fs-sink.cc:159-195`), and drive the derived component alone, two levels deep, with a test that fails when the override is removed.
