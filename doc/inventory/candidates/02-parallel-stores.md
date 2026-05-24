# Parallel store implementations

Candidates 11-16. All six VALID against `src/libstore/local-overlay-store.cc`,
`restricted-store.cc`, `dummy-store.cc`, `binary-cache-store.cc`,
`local-binary-cache-store.cc`, `http-binary-cache-store.cc`,
`s3-binary-cache-store.cc`, `uds-remote-store.cc`, `ssh-store.cc`,
`legacy-ssh-store.cc`, and `remote-store.cc`.

| # | Verdict | Effort |
| - | ------- | ------ |
| 11 | VALID | small |
| 12 | VALID | medium |
| 13 | VALID | small |
| 14 | VALID | trivial |
| 15 | VALID | small |
| 16 | VALID | medium |

---

11. **`LocalOverlayStore` repeats upper-vs-lower lookup pattern across at least ten methods.** Methods that consult both stores: the three pure-fall-through methods `queryPathInfoUncached`, `queryRealisationUncached`, `queryPathFromHashPart` (try upper, fall through to lower); the two union methods `queryReferrers`, `queryValidDerivers` (combine results from both); and the copy-up methods `registerDrvOutput`, `registerValidPaths`, and `isValidPathUncached` (which materialises lower-store metadata into upper after a hit by recurring on `references` and calling `LocalStore::registerValidPath`). `deleteStorePath` and `optimiseStore` additionally branch on `lowerStore->isValidPath(...)` to decide between upper-only delete vs overlay delete (these are GC-side rather than read-side, so they don't quite fit the lookup pattern but still cross both stores). The two callback variants build a chained continuation through `Callback`/captured `callbackPtr`; the synchronous fall-through variants short-circuit on the upper hit. A helper for the synchronous fall-through shape would consolidate `queryPathFromHashPart` and the lookup half of `isValidPathUncached` (and partly the union pair).
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. Pitfall: `isValidPathUncached`'s recursive materialisation must keep using the public `isValidPath` (not the uncached form) so the in-process cache layer keeps working. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libstore` (extracted a file-local `tryUpperFallLower<UpperLookup, LowerLookup>` template returning `std::pair<ResultT, bool /*fellThroughToLower*/>` from a pair of callables. `queryPathFromHashPart` consumes only `.first`; `isValidPathUncached` destructures both and gates the lower-store materialisation on `fellThroughToLower && res`. The pair-return shape avoids the side-effect-via-captured-flag idiom that the first iteration used. The two callback-shaped methods (`queryPathInfoUncached`, `queryRealisationUncached`) and the two union methods (`queryReferrers`, `queryValidDerivers`) keep their own bodies — different control flow.)

12. **Three "store wrapper" implementations with no shared abstract base.** `LocalOverlayStore` (delegating to `lowerStore`), `RestrictedStore` (delegating to `next`), `DummyStoreImpl` (no delegation; in-memory). Each redeclares large portions of the `Store` virtual surface. A "DelegatingStore" CRTP or non-virtual base would deduplicate.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. `DummyStoreImpl` doesn't share the delegation pattern but does redeclare the same surface; a `DelegatingStore` CRTP covers two of three cleanly. Effort: medium.

13. **Binary-cache subclasses duplicate the upsertFile/fileExists/getFile triple.** `LocalBinaryCacheStore`, `HttpBinaryCacheStore`, `S3BinaryCacheStore` each provide their own implementation, but the surrounding logic is mostly cloned: build paths from URL, wrap errors as a per-store `MakeError(UploadTo*)`, handle `NotFound`/`Forbidden` specifically. `HttpBinaryCacheStore::upsertFile` and `S3BinaryCacheStore::upsertFile` share an identical "if compress then compress + Content-Encoding else dispatch upload" skeleton.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. Pitfall: `S3BinaryCacheStore` adds an MD5-and-size pre-check before calling `upload`/`uploadMultipart`; the helper must let subclasses interpose around the upload, not just after it. Effort: small.

14. **`RemoteFSAccessor` constructed identically by `RemoteStore::getRemoteFSAccessor` and `BinaryCacheStore::getRemoteFSAccessor`.** Both pass the `requireValidPath` flag; only difference is `BinaryCacheStore` plumbs through `config.localNarCache`. The corresponding `getFSAccessor` overrides are mechanical wrappers in both. ~10 lines could move into `Store`.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore` (lifted `getRemoteFSAccessor` to `Store` plus a virtual `getLocalNarCacheDir()` hook; left the trivial 1-line `getFSAccessor` overrides per-class)

15. **`UDSRemoteStore` and `MountedSSHStore` mix `RemoteStore` with `LocalFSStore` identically.** Both override `getFSAccessor`/`narFromPath` with the same delegate-to-`LocalFSStore` calls. The only meaningful divergence is the GC-root strategy: `UDSRemoteStore` sends `WorkerProto::Op::AddIndirectRoot`; `MountedSSHStore` sends `WorkerProto::Op::AddPermRoot`.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. A `RemoteFSStoreMixin` policy class could absorb both. Pitfall: virtual inheritance of `Store` means the mixin must `virtual`-inherit `LocalFSStore` and `RemoteStore` to keep the diamond intact for unambiguous override resolution. Effort: small.

16. **Three SSH-store classes share `CommonSSHStoreConfig` but each rolls its own `Connection`.** `LegacySSHStore::Connection`, `SSHStore::Connection`, and the worker-proto `Connection` inside `RemoteStore` each carry a `unique_ptr<SSHMaster::Connection> sshConn` and pipe wiring. `LegacySSHStore` reimplements its own `Pool<Connection>`, `connect`, `flushBadConnections`-equivalent (`good` flag), and stat reporting (`getConnectionStats`/`getConnectionPid`) instead of leveraging `RemoteStore`.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. The `LegacySSHStore`-private `Pool<Connection>` is the largest source of duplication; lifting it into a `PooledClientStore` non-template base (alongside an `SshConnection` mixin shared by `RemoteStore::Connection` and `ServeProto::BasicClientConnection`) is the natural factoring. Effort: medium — aligning the legacy serve-protocol pool with `RemoteStore::ConnectionHandle` may shift error semantics.
