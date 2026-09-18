---
synopsis: "Store objects are addressed by their git tree hash"
prs: []
---

A store object's content hash is now its *object hash*: the git identifier of its root object, computed with SHA-256 and rendered `git:sha256:<64 hex digits>`.
For a directory it is the git tree id; for a plain (non-executable) file it is the git blob id; for an executable file or a symlink it is the id of a one-entry git tree whose single entry is named `.` and carries the mode and the blob, so that the three kinds of non-directory root stay distinct (see [Git content addressing](@docroot@/store/file-system-object/content-address.md#git)).
The hash of the [NAR serialisation](@docroot@/store/file-system-object/content-address.md#serial-nix-archive) is no longer stored: it is a function of the tree and is computed by one walk of the object when an older client, format or file asks for it.

The forms this changes:

- **Path info.** `narHash` is replaced by `objectHash`; `narSize` is kept.
  Path-info JSON format 4 (`nix path-info --json --json-format 4`) has `objectHash` (required) and `narHash` (optional, present when a peer or file asserted one).
  Formats 1 to 3 are still written; their `narHash` is computed on demand.

- **The store database.** Schema version 11.
  A row registered by an older Nix holds a NAR hash and is migrated, by one walk of the path, the first time it is queried (the row is written back unless the store is opened read-only); the walk checks that NAR hash on the way, as `nix-store --load-db` does, and a path whose files no longer match it is not migrated — it is answered from the row as before, `nix store verify`, `nix-store --verify-path` and `nix-store --verify --check-contents` report it as modified, and `--repair` restores it. A row whose files are gone is answered from the database too and is removed by `nix-store --verify`.
  `nix store migrate` migrates every row at once; it is safe to interrupt and to run again.
  An older Nix cannot open a schema-11 store.
  Until every row is migrated, the garbage collector reclaims no space from the object store (see *The object store* below): it deletes dead store paths as before, but sweeps no object-store tree or blob, and says so.

- **`nix-store --dump-db` and `--load-db`.** A dump written by this Nix carries each path's object hash, rendered `git:sha256:…`, on the line where the NAR hash was; an older Nix cannot load it.
  A dump written by an older Nix is loaded: each record's NAR hash is checked by one walk of the path, the same walk that computes the object hash the row needs — one read of every byte of the restored store, paid once — and a record whose path does not hash to it is reported (`path '…' was modified! expected hash '…', got '…'; not registered`) and skipped; the other records are registered, and the command exits with status 1 when any record was skipped.
  `nix-store --register-validity --hash-given` reads records the same way.

- **Binary caches.** A `.narinfo` written by this Nix carries `ObjectHash:` beside `NarHash:` (the NAR hash comes from the upload stream at no extra cost) and, when the cache signs its uploads with its own keys (`secret-key` or `secret-keys` on the cache), one signature per key over each fingerprint version (see *Signatures* below).
  Either hash field suffices when reading: a `.narinfo` from an older cache carrying only `NarHash:` is accepted, and the NAR hash it asserts is verified as the NAR is received.
  Older clients use `NarHash:` and version-1 signatures and ignore the field and the signature version they do not know, so they read a cache written by this Nix unchanged **when the cache signs at upload or `nix store sign` has been run on it** — with one exception, the git-named sources (see *Sources* and *Older peers* below): an older client parses a `.narinfo` whose `CA:` is `fixed:git:…` only with its `git-hashing` experimental feature enabled, and otherwise fails with `error: experimental Nix feature 'git-hashing' is disabled; add '--extra-experimental-features git-hashing' to enable it`.
  They do not otherwise: a path signed at build time (`secret-key-files` on the local store) carries a version-2 signature only, `nix copy --to` a cache that has no key of its own carries it unchanged, and an older client that requires signatures (`require-sigs`) refuses it.
  The remedy is `nix store sign --store <cache-url> --key-file <key> --all` on the cache, which adds the version-1 signature and works on any cache, including one an older Nix wrote (`--all` enumerates a `file://` cache; for another kind of cache, name the paths).
  The per-user cache of `.narinfo` lookups gains a column for the object hash and moves from `~/.cache/nix/binary-cache-v8.sqlite` to `binary-cache-v9.sqlite`: it goes cold once and is refilled as caches are queried; the `v8` file is not read and can be deleted.

- **Protocols.** The worker protocol gains the feature `object-hash`: when both sides have it, path info carries the object hash (and any asserted NAR hash); to a client without it the daemon supplies the NAR hash, computed by one walk of the path **per query** — there is no cache, so an older client pays that walk every time it asks about a path.
  The serve protocol is version 2.9 with the same rule.

- **Older peers and git-named sources.** Every source this Nix adds is content-addressed under the git method (`fixed:git:sha256:…`, *Sources* below).
  A Nix release before this one (2.35 and earlier, and 2.36 pre-releases before this change) parses that method only when its `git-hashing` experimental feature is enabled — in `nix.conf` (`extra-experimental-features = git-hashing`) or on the command line — and otherwise fails with `error: experimental Nix feature 'git-hashing' is disabled; add '--extra-experimental-features git-hashing' to enable it`.
  This is so in every direction: an older client asking a new daemon about a source (`nix path-info`, `nix copy --from`), an older client reading a `.narinfo` this Nix wrote for a source, and a new client adding a source to an older daemon (`builtins.path`, `fetchTree`, a `path:` input: the daemon refuses the `AddToStore`).
  Paths under the other methods — build outputs, `nix store add --mode nar`, fixed-output derivations — are unaffected.
  Nothing in this Nix can change what an older binary accepts; set the feature on the older side.

- **Signatures.** The fingerprint is version 2: `2;<store path>;<object hash>;<references>`.
  `nix store sign` signs the version-2 fingerprint when the path's description carries an object hash, and the version-1 fingerprint (`1;<store path>;<NAR hash>;<NAR size>;<references>`) whenever its NAR size is known — the NAR hash asserted by the description, as a `.narinfo` does, or else computed by one walk of the path — so that older clients, which know only version 1, verify what it signs, and so that it works on a cache an older Nix wrote, whose paths have no object hash and get the version-1 signature alone.
  The upload to a binary cache that has its own keys signs both fingerprints the same way, the NAR hash coming from the upload stream.
  Signing at build time (`secret-key-files` on the local store) signs version 2 only: no NAR hash exists there and a walk of every output is not paid.
  A version-1 signature is verified against the path's NAR hash: the one asserted for it (a path described by an older cache or peer, or a database row not yet migrated), or else — for a locally held path whose row holds the object hash alone — one computed by a walk of the path, made at most once per path and only when a trusted key's version-1 signature is met.
  So a version-1 signature made *before the upgrade* on a locally held path keeps verifying after its row is migrated, at the cost of that walk; `nix store copy-sigs` copies an older cache's version-1 signatures for such a path (the walked NAR hash against the cache's `NarHash:` establishes that both describe the same bytes), and `nix store verify --substituter` checks them. Content-addressed paths need no signature.
  An older Nix does not verify version-2 signatures; it counts only the signatures it can verify, so the version-2 signature is ignored, not rejected.

- **Sources.** Every source added to the store as a tree is named under the git content-address method by its tree hash: `builtins.path` and `builtins.filterSource` (with the default `recursive = true`), path values coerced to strings, flake inputs, `builtins.fetchTree`, `fetchGit`, `fetchTarball`, `path:` inputs, and a dirty Mercurial checkout.
  The store paths of all such sources therefore differ from those of earlier releases, and so do the paths of everything built from them; a binary cache holding a source under its earlier path does not serve it under the new one.
  Exceptions, which keep the NAR method and their earlier paths: a source with references (`builtins.path` on a store path with references), a Mercurial input at a committed revision, profiles, and `nix store make-content-addressed`.
  The explicit methods are unchanged: `builtins.path { recursive = false; }`, `nix store add --mode`, and fixed-output derivations in `recursive`/`flat` mode.
  `outputHashMode = "recursive"` and the hashes nixpkgs records under it are untouched.
  A bare executable file or symlink, which the git method used to refuse as a root, is now addressable under it (see the manual section linked above).
  An earlier Nix that did address such a root under the git method (`nix store add --mode git` on an executable file or a symlink) named it by the blob id of its bytes, the mode discarded; this Nix names it by the id of the synthetic one-entry tree, so the two disagree on that path's name, and a copy of such a path from an older store or cache into this one -- `nix copy`, substitution, anything that carries the path's content address -- is refused with `ca hash mismatch importing path '…'; specified: sha256:<blob id>; got: sha256:<tree id>`.
  (`nix-store --import` carries no content address and admits the path as it is.)
  Re-add the source under this Nix to obtain its new name.

- **Flakes.** A locked input carries `treeHash` (an SRI SHA-256 string, the object hash of the input's root); `flake.lock` is version 8 and no longer writes `narHash`.
  Lock files of versions 5 to 7 are still read: a node's `narHash` is an assertion, verified by one walk of the fetched tree the first time that input is fetched, after which the node carries `treeHash`.
  A lock file is written as version 8 whenever it is written at all; a lock whose inputs are all unchanged is not rewritten by itself, so a version-8 lock may still carry a node's `narHash` alone, and every reader accepts either.
  `treeHash` and `narHash` are both accepted in flake reference URLs (`?treeHash=`, `?narHash=`) and as input attributes.
  An older Nix cannot read a version-8 lock file.

- **The language.** `builtins.path` and `builtins.fetchTarball` accept a `treeHash` argument, the assertion that names the tree and costs no walk; `sha256` is still accepted and verified by a walk of the fetched tree, once per tree.
  A `sha256` finds a tree already in the store without fetching only after a `sha256`-pinned fetch of that tree has once been verified; a `treeHash` names it directly.
  `builtins.fetchTree` and `fetchGit` return `treeHash`; `narHash` remains in their result and is computed from the tree when a program forces it.
  `fetchTarball` returns a path string, as before; `fetchMercurial` returns neither hash.
  `fetchTree { narHash = ...; }` is accepted and verified the same way.

- **Commands and builders.** `nix-store --query --hash` prints `git:sha256:<hex>` (a NAR hash rendering only for a path described by a peer that carried no object hash).
  `nix flake prefetch` prints the tree hash as `hash`.
  `nix store verify`, `nix-store --verify-path` and `nix-store --verify --check-contents` recompute the object hash.
  `exportReferencesGraph` under `__structuredAttrs` lists `objectHash` where it listed `narHash`.

- **Experimental features and settings.** The `git-hashing` experimental feature is stabilised and removed (see its own release note): the `git` content-address method (for instance `nix store add --mode git`), `outputHashMode = "git"` on derivations and the reading of git object streams need no feature.

- **The git method is SHA-256 only.** The experimental feature admitted SHA-1 as well, so that a fixed-output derivation could copy its hash from `git rev-parse` of an ordinary repository; that form is removed.
  A fixed-output, floating or impure derivation with `outputHashMode = "git"` and any algorithm but SHA-256 is refused at instantiation; `nix store add --mode git --hash-algo sha1` and `nix hash path --mode git --algo sha1` are refused; each message names the remedy: compute the SHA-256 address with `nix hash path --mode git <path>` and pin that.
  A `.drv` that already names the SHA-1 form (written by an earlier Nix, or added with `nix derivation add`) is still read and shown, and is refused when it is built.
  A store object an earlier Nix addressed under the SHA-1 form is read wherever it is described -- its database row, a `.narinfo`, `nix path-info`, the collector and `nix-store --verify --check-contents` all work on a store holding one -- but it cannot be copied into a **local** store of this Nix (`nix copy` to one, substitution into one): the receiving local store checks the content address first and cannot compute a SHA-1 one. A binary cache accepts it (`nix copy --to file://…` succeeds and the cache serves its `CA: fixed:git:sha1:…`), and a local store of this Nix then refuses it at substitution.
  Re-add the source with this Nix to obtain its SHA-256 name, or carry the path with `nix-store --export | nix-store --import`, which asserts no content address.

- **The object store and the tarball cache.** The object store is unconditional: from the moment a path is added, its regular files are hard links into `<store>/.objects/` (`blobs/`, `blobs-x/`, `trees/`, `tmp/`): git objects under SHA-256, one file per blob and mode, so two files with the same bytes that differ in the executable bit never share an inode.
  The `auto-optimise-store` setting has no meaning any more; it and its `--auto-optimise-store` flag are still accepted and ignored; when set to `true` they are answered with one warning per top-level process saying that the object store shares identical files unconditionally, and `false` is silent. The setting is not listed by `nix config show` (it is by `nix config show --json` and by name).
  A file whose blob the store already holds is linked and not written when it is under 1 MiB; above that it is written and then replaced by a link to the store's file.
  `max-free` and `--max-freed` count the bytes a deletion frees on the disk: a regular file of a deleted path whose blob another live path still links frees nothing until the next whole-store collection sweeps the blob, so a collection under `min-free`/`max-free` reclaims less per round than an unshared store would, and `nix store delete` says when the bytes it counts go.
  The `.links` directory is no longer used; `nix-store --optimise` migrates a store written by an older Nix into the object store and removes `.links` (the garbage collector removes it at any whole-store collection as well), and on a store this Nix has written all along it links nothing.
  Limits: a file the store cannot link is left as written and shares nothing — a blob at the filesystem's hard-link limit, a file under `.app/Contents` on macOS (which optimisation skipped before as well), every file on a filesystem that cannot hard-link, and every file on Windows, where the object store shares nothing; directories are never hard-linked, so trees share in the object store but not on disk.
  The tarball cache moved to `~/.cache/nix/tarball-cache-v3`, a SHA-256 git repository whose object identifiers are the store's; the earlier cache is not read, so every tarball is unpacked once more.

What to do after upgrading:

- Nothing is required for correctness.
  Running `nix store migrate` once makes the first query of every path (and so the first `nix-collect-garbage`) cheap; it is safe to interrupt and to run again.
- Relock flakes (`nix flake lock` or `nix flake update`) when convenient; do so only for flakes that no older Nix must read, since a version-8 lock is not readable by older versions.
- Binary caches written by an older Nix need no change: they are read through the asserted NAR hash.
  Older clients that must read the sources this Nix uploads, or talk to a daemon running this Nix about them, need `extra-experimental-features = git-hashing` (*Older peers and git-named sources* above).
  Binary caches written by this Nix carry both hashes; they carry both signature versions only if the cache signs its uploads with its own keys.
  A cache that holds paths signed only at build time (version-2 signatures only) must be signed with `nix store sign --store <cache-url> --key-file <key> --all` before older clients that require signatures accept it.
