# Cache-key construction scattered across modules

Candidates 69-71. #69 and #70 fully VALID; #71 PARTIALLY VALID — the
NarInfo and Realisation row marshalling differ enough that a generic base
helps less than the original framing suggested.

| # | Verdict | Effort |
| - | ------- | ------ |
| 69 | VALID | medium |
| 70 | VALID | small |
| 71 | PARTIALLY VALID | medium |

---

69. **Per-fetcher `Cache::Key` schemas with no shared helper.** Each scheme builds `Cache::Key{<domain>, <attrs>}` ad hoc. Domains seen: `sourcePathToHash` (`fetch-to-store.cc`), `gitLastModified`/`gitRevCount` (`git.cc`), `gitRevToTreeHash`/`gitRevToLastModified` (`github.cc`), `treeHashToNarHash` (`git-utils.cc`), `hgRefToRev`/`hgRev` (`mercurial.cc`), `tarball`/`file` (`tarball.cc`). The `Cache` class is shared but each scheme picks its own attribute schema.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID. A typed `CacheKey<Domain>` template — one type per domain with a known attribute schema — would centralise the schema and enable static checking. Effort: medium.

70. **`makeSourcePathToHashCacheKey` is called from three places with slightly different shapes.** From `Input::getAccessorUnchecked`, `PathInputScheme::getAccessor`, and `fetch-to-store.cc`. They could share a helper.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID. Effort: small.

71. **NarInfoDiskCache key plumbing.** `lookupNarInfo`/`upsertNarInfo`/`upsertAbsentNarInfo` and `lookupRealisation`/`upsertRealisation`/`upsertAbsentRealisation` follow identical "TTL + present-bit + reconstruct" patterns. Could share a generic "TTL-cached lookup" base.
    - ../verified/08-libstore-remote.md
    - **Validation:** PARTIALLY VALID. The "TTL + present-bit" envelope is shared, but row marshalling differs structurally (NarInfo has many fields; Realisation is a single JSON blob). A generic base helps for the lookup/insert envelope only; the row codec stays per-type. Effort: medium.
