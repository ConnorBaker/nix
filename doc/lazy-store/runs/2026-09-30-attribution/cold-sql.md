# Pinned cold32 `path:` SQLite statement attribution

Question: What SQLite statement families run behind the extra cold-path local database syscalls? The sealed five-pair benchmark remains the timing authority: f307 versus 8aad cold32 path wall −23.270%, CPU +48.607% (about +1.30 s system and +0.31 s user in its paired resource reduction). This collector run is diagnostic, not a timing comparison.

One fresh-state evaluation used the exact f307 release binary (SHA256 `9c51c263ae7b3f45147180532b8f2ba7b53a2840b86d4039803173ed2fe26661`), fixture, configuration and 32-attribute expression (expression SHA256 `e72731fe38bed1462919c09d624c5fad2e5d9980e311f21a67a9de5b5b4f574a`). It exited 0 in 9.00 instrumented seconds, with load1 0.39→0.93. Full output SHA256 `d1dd4f18ce647e0573b2e52b74c8cd96a08c0b7ca5a05cb2cfc40ebf3daaf440` exactly matches the earlier pinned f307 result. The private database had 3,694 ValidPaths, 56,335 Blobs, 37,121 AuxValues and 3,694 Ingestions, with 4,096-byte pages and 5,250 pages. No source was changed.

The historical collector-only run below intercepts SQLite prepare/step/reset/finalize/exec and libc `fcntl64`/`fcntl`/`pwrite64`/`pwrite`. Counts below are *inside* an interposed `sqlite3_step`, keyed by its unexpanded SQL template and local SQLite connection; reset counts separately establish one `LookupBlob` use per completed query. Every measured `pwrite` (without 64) count was zero.

| Local SQLite statement family | Steps | `fcntl64` inside steps | `pwrite64` inside steps |
|---|---:|---:|---:|
| `LookupBlob` SELECT | 56,900 (56,336 DONE; 564 ROW) | **112,682** | 0 |
| `COMMIT` | 3,696 | 7,412 | **165,396** |
| `InsertBlob` | 56,335 | 0 | **77,606** |
| `InsertAuxiliary` | 37,121 | 0 | **4,608** |
| `NewIngestion` | 3,694 | 7,388 | 0 |
| Other local statements | — | 28,567 | 76 |
| **Local SQLite total, all intercepted operations** | — | **156,049** | **247,686** |

`LookupBlob` had **56,336 query uses**, each with its own step/reset lifetime. Its 112,682 `fcntl64` calls are **2 per use plus 10** and 72.2% of this run's local SQLite `fcntl64`. This directly identifies the read-lock source route: `GitObjectStore::{link,place}` → `LocalStore::blobIndex.lookup` → `SQLiteStmt::Use::step`; the query joins `Blobs`, `Ingestions`, and `ValidPaths`. The prior retained matched FD traces counted local SQLite SHM `fcntl` 43,299→156,044 (+112,745) between 8aad and f307, and their byte-124 read/unlock lock deltas were +56,349/+56,357. The collector's local SQLite `fcntl64` total differs from that earlier f307 SHM total by only **5**; its `LookupBlob` count differs from the *cross-run* delta by **63**. These are separate runs and file/socket classifications, so the small residual is not an exact subtraction proof. A separate 100-miss WAL microcheck reproduced 202 `fcntl64` inside 100 missing-key SELECT steps.

**Same-run reconciliation:** A later exact f307 cold32 run paired strace with FD/length-classified SQL counters. Every local SQLite write count, successful byte count and error count agrees: **173,437 writes** (166,661 WAL, 6,694 main DB, 80 SHM, 2 journal), with zero unparsed or unfinished calls. The historical table's 247,686 writes are therefore a cross-run difference, chiefly `InsertBlob` (77,606 → 3,116); its cause remains unproved. The full output and row census match. A 53-write residual is outside local SQLite, so universal interposer coverage is not claimed. `LookupBlob` again measures two locks per query plus ten (112,696 locks, 56,343 queries). The [archived same-run report](/home/connorbaker/Benchmarks/nix-performance-fixes/attribution-20260930/cold-sql/READOUT.md) binds the raw trace, counters, manifest and state-retirement receipt. These counts assign no causal seconds to the CPU regression.

Historical collector evidence: `manifest.json`, `calls.tsv`, `stdout.json`, `stderr` and `state-retirement-receipt.json` in `/tmp/nix-sql-statement-attribution-20260930`. The receipt binds raw-call SHA256 `98a67735b2266861fa52107c1b455c725531d634a411736ccaa00e1a3a351c9c` and manifest SHA256 `8cbec7fd59bf3cf48ada9fa2b0829411096db38e34f68b1a3e2c81cef00376c8`, records the database counts and 56,568-file/3-symlink census, and verifies private-state removal. The later same-run evidence and its probe sources are in the linked archive.

## Lock-removal domain

A holder query acquires candidate metadata; physical validation still determines whether that candidate can supply bytes. The relevant finite cases are:

| State/event | Required behavior | Batching constraint |
|---|---|---|
| Same-ingestion holder exists | Use the private witness | Already avoids SQLite. |
| Sound published holder exists | Link eligible buffered contents without writing or firing `regularFileCreated` | Deferring lookup until after the write violates the explicit ingestion contract. |
| Candidate is stale, removed or corrupt | Check the physical witness, reject it and try another | Acquired SQL metadata does not freeze the physical file. |
| Candidate is absent or another publisher commits | Preserve the chosen acquisition meaning and reconcile before publication | Today's individual-query timing is operational, not independently a law; an ingestion-wide negative snapshot needs a justified interpretation. |
| Link limit, ENOSPC or other I/O failure | Preserve documented refusal and error handling | Final reconciliation cannot undo earlier space consumption. |
| Callback or another thread uses the same connection | Preserve reentry, publication and transaction retry | Holding `_state` through traversal deadlocks callbacks; releasing it leaves a connection-global transaction. Nested `BEGIN` fails, and stale-candidate deletion cannot refresh a failed read-to-write snapshot promotion by retrying only `DELETE`. |

The actual constraints are [01 §9.10](../../01-specification.md)'s buffered no-write-on-reuse contract and its prohibition on transactions spanning source/destination callbacks. Current `BufferedRegularFile` carries one leaf; child continuations already read, hash and place it. No pre-placement batch exists. A compound query over already available exact keys is possible in principle, with acquisition ending before physical checks; exposing such a batch requires deriving the traversal and callback semantics, rather than wrapping today's walk in a transaction. The finite census finds only nine streamed files above the 1 MiB limit, so deferring their after-write lookup cannot eliminate the roughly 56,000 queries. No safe subtraction was established; this is not a claim that every representation needs two locks per file.
