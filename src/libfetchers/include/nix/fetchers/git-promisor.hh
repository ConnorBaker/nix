#pragma once
///@file
///
/// Nix-side Git protocol-v2 upload-pack client.
///
/// libgit2 has no native partial-clone fetch (issue libgit2#5564 open
/// since 2020); the smart-protocol want-list serialiser does not emit
/// `filter` lines and is not exported. Wrapping libgit2's smart
/// subtransport doesn't help either — capability negotiation happens
/// above the subtransport, so by the time we'd see bytes, the wants
/// have been frozen.
///
/// So we own a small protocol-v2 client over Nix's `FileTransfer` HTTPS
/// layer, and feed returned packs through `git_indexer_*` outside any
/// libgit2 ODB callback. The seam is `GitPromisorProvider::ensureObjects`:
/// before libgit2 is asked to read an object, the provider checks
/// whether it's local; if not, it issues a single coalesced
/// `command=fetch` with `want <oid>`/`filter blob:none`/`done`,
/// indexes the returned pack into the on-disk repo, refreshes the
/// ODB, and returns. The `GitSourceAccessor::readBlob` lock split (track G)
/// already separates path-to-OID from byte-streaming, so this
/// integrates without touching the ODB callback path that libgit2's
/// own mutex protects.

#include "nix/util/error.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <chrono>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

class FileTransfer;
struct GitRepo;
struct Source;

/* ---------- pkt-line framing (Git wire format) ----------
 *
 * Exposed for testing. Each packet is `<4 hex digits><payload>`.
 * Three reserved sizes are control packets, not data:
 *   "0000" — flush-pkt
 *   "0001" — delim-pkt (v2)
 *   "0002" — response-end (v2)
 * Otherwise the 4-digit value is the *total* packet length including
 * the 4-digit header.
 */

namespace pkt {

constexpr size_t kHeaderLen = 4;

/** Append a data packet with the given payload. */
void appendData(std::string & buf, std::string_view payload);

/** Append a flush-pkt (`0000`). */
void appendFlush(std::string & buf);

/** Append a delim-pkt (`0001`). */
void appendDelim(std::string & buf);

struct Line
{
    enum class Kind { Flush, Delim, ResponseEnd, Data };
    Kind kind;
    std::string_view payload; // populated only for Kind::Data
};

/** Parse one pkt-line from `cursor` and advance `cursor` past it. */
Line readLine(std::string_view & cursor);

/**
 * Read one pkt-line from a streaming `Source` (exact reads; throws at
 * EOF mid-packet). For `Kind::Data` the returned `Line::payload` view
 * is backed by `owned`, which the caller must keep alive until the
 * payload is consumed. Used by the ssh transport, where the
 * `git-upload-pack` response arrives incrementally over a pipe rather
 * than as one in-memory buffer.
 */
Line readLine(Source & source, std::string & owned);

} // namespace pkt

/**
 * `command=fetch` argument variant. Today only `Blobless` (the
 * `filter blob:none` form Git's partial-clone uses) is implemented;
 * future filters (sparse:oid, tree:0) compose by adding entries.
 */
enum struct FetchFilter {
    None,
    Blobless,
};

/**
 * Single-quote `remotePath` the way Git quotes the path argument to
 * `git-upload-pack` (wrap in `'…'`, escaping embedded quotes as
 * `'\''`). This is the second argv word the ssh transport sends.
 */
std::string gitUploadPackCommandArg(std::string_view remotePath);

/**
 * The full remote command string the ssh transport sends to run
 * `git-upload-pack` for `remotePath`. It MUST be a BARE
 * `git-upload-pack '<path>'`: Git forges restrict the ssh login to
 * `git-shell`, which allowlists only `git-upload-pack` /
 * `git-receive-pack` / `git-upload-archive` as the first word, so any
 * prefix (e.g. an inline `env VAR=… `) is rejected. The protocol
 * version is therefore carried out-of-band via ssh `SetEnv` (see
 * `gitProtocolSetEnvArg`), never inlined here.
 *
 * Exposed so this forge-safety property is unit-testable without an
 * ssh server (it was a field regression against GitHub Enterprise).
 */
std::string gitUploadPackCommand(std::string_view remotePath);

/**
 * The ssh option that carries the Git protocol version out-of-band to
 * the remote (`-oSetEnv=GIT_PROTOCOL=version=2`), so the remote command
 * stays a bare `git-upload-pack` (see `gitUploadPackCommand`). Exposed
 * for the same regression test.
 */
std::string gitProtocolSetEnvArg();

struct GitPromisorProvider
{
    virtual ~GitPromisorProvider() = default;

    /**
     * Probe the remote's protocol-v2 capabilities. Returns true if
     * the server advertises `version 2` and `fetch=...filter`.
     */
    virtual bool supportsFilteredFetch() = 0;

    /**
     * Ensure the named OIDs are present in the repository's ODB.
     *
     * Implementation: emits one `command=fetch want <oid>+ done` over
     * `FileTransfer` POST, streams the response through
     * `git_indexer_*`, then calls `git_odb_refresh`. All work happens
     * outside any libgit2 ODB callback — the flow is "fetch, index,
     * refresh" before libgit2 is asked to read.
     *
     * Coalesce by passing many OIDs in one call; per-blob fetches are
     * what makes naive partial-clone slower than full clone (one
     * round-trip per blob, hours of wall-clock on cold nixpkgs eval at
     * typical RTT).
     */
    virtual void ensureObjects(std::span<const Hash> wants, FetchFilter filter = FetchFilter::Blobless) = 0;
};

/**
 * Open a promisor provider against `url` whose pack writes land in
 * `repoPath`. The provider holds no libgit2 handles itself; it owns
 * only the URL plus an `FileTransfer` reference.
 */
ref<GitPromisorProvider> makeGitPromisorProvider(std::string url, std::filesystem::path repoPath);

namespace fetchers {

/**
 * Result of a per-URL protocol-v2 capability probe. `supportsFilter`
 * is true iff the remote advertises both `version 2` and a `fetch`
 * capability that includes `filter`. `probedAt` records when the
 * probe ran for TTL bookkeeping.
 *
 * `probeFailed` distinguishes a *negative* result (the remote answered
 * and does not advertise `filter`) from a *transient* one (the probe
 * round-trip itself threw — timeout/DNS/5xx/ssh hiccup). Both set
 * `supportsFilter = false`, but a transient failure must not be cached
 * as authoritative for the full TTL, nor used to detach the backfill
 * provider from a clone that is *already* partial (where a per-blob
 * fetch is strictly better than no provider at all).
 */
struct V2ProbeResult
{
    bool supportsFilter = false;
    bool probeFailed = false;
    std::chrono::steady_clock::time_point probedAt;
};

/**
 * Returns the cached protocol-v2 capabilities for `url`. If the
 * cached entry is missing or older than the TTL (30 minutes by
 * default), the probe runs again and the cache is updated.
 *
 * This is a per-URL TTL cache. Multiple `GitInputScheme` invocations
 * within the same process for the same partial-clone remote share a
 * single capability probe.
 */
V2ProbeResult gitV2CapsCache(const std::string & url);

/**
 * Test seam: the function used to actually probe a remote's
 * capabilities. Defaults to constructing a `GitPromisorProvider` and
 * calling `supportsFilteredFetch()`. Tests can swap this to a stub
 * that increments a counter and returns a deterministic result, so
 * we can verify the TTL cache deduplicates repeat calls.
 *
 * Test code is responsible for restoring the previous probe before
 * exiting the test (and clearing the cache via
 * `gitV2CapsCacheClearForTest`).
 */
using V2ProbeFn = std::function<bool(const std::string &)>;
V2ProbeFn setV2ProbeForTest(V2ProbeFn fn);

/** Test seam: clear the per-URL TTL cache. */
void gitV2CapsCacheClearForTest();

} // namespace fetchers

} // namespace nix
