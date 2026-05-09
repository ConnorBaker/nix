#pragma once
/// store/trace-value-types.hh — Namespace-scope value types for the
/// trace-storage interface.
///
/// These types live outside `TraceStore` so that lightweight consumers
/// (the upcoming Recorder / Verifier / TraceBackend, the free-function
/// codec + resolver in trace-result-codec.hh + trace-resolve.hh, and
/// `nix eval-info` reporters) can depend on them without pulling the
/// SQLite-backed storage surface (`nix/store/sqlite.hh` + 28
/// SQLiteStmt members).
///
/// Promoted from nested-in-`TraceStore` per rearchitecture-proposal.md
/// §14 step 9. `TraceStore` keeps `using` aliases so existing callers
/// compile unchanged; once the abstract `TraceStorage` base lands,
/// these types become the single authoritative home and the aliases
/// go away.

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tagged.hh"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nix::eval_trace {

// IDs declared here so callers of this header get ResultId /
// DepKeySetId / TraceId without pulling trace-store.hh.
using TraceId = Tagged<struct TraceIdTag_, uint32_t>;
using ResultId = Tagged<struct ResultIdTag_, uint32_t>;
using DepKeySetId = Tagged<struct DepKeySetIdTag_, uint32_t>;

/// Lightweight index entry — no payload strings. Stored in currentNodeIndex.
struct CurrentNodeRef {
    TraceId traceId;
    ResultId resultId;
    NodeStamp nodeStamp;

    bool operator==(const CurrentNodeRef &) const = default;
};

/// Result payload loaded on demand from Results table.
struct ResultPayload {
    ResultKind type{};
    uint32_t encodingVersion = kSemanticResultEncodingVersion;
    std::string payload;
    std::string auxContext;

    bool operator==(const ResultPayload &) const = default;
};

/// Metadata-only per-trace session cache. Header rows are cached separately
/// from full dep vectors, so the store never manufactures partially-valid
/// trace entries with sentinel hash fields.
struct TraceHeader {
    TraceHash traceHash;
    DepKeySetHash keySetHash;
    DepKeySetId depKeySetId;

    bool operator==(const TraceHeader &) const = default;
};

/// Trace blobs loaded from storage as a unit. `keysBlob` and
/// `valuesBlob` are both `NOT NULL` at the schema level (may be
/// empty), so they are non-optional; presence of the blob
/// wrapper itself indicates the trace row was found.
struct TraceBlobs {
    TraceHeader header;
    std::vector<uint8_t> keysBlob;
    std::vector<uint8_t> valuesBlob;
};

/// Lightweight index entry for a History row (no NodeStamp — History
/// rows aren't per-session and have no stamp, unlike CurrentNodeRef
/// which always carries one).
struct HistoryNodeRef {
    TraceId traceId;
    ResultId resultId;

    bool operator==(const HistoryNodeRef &) const = default;
};

/// A single entry from the History table. Used by both recovery
/// bootstrap (scanHistory) and the structural-variant scan.
struct HistoryEntry {
    DepKeySetId depKeySetId{};
    TraceId traceId{};
    ResultId resultId{};
    TraceHash traceHash{};

    bool operator==(const HistoryEntry &) const = default;
};

struct VerifyResult {
    CachedResult value;
    TraceId traceId;
};

struct RecordResult {
    TraceId traceId;

    bool operator==(const RecordResult &) const = default;
};

/// Entry from the SessionRuntimeRoots table.
struct RuntimeRootRecord {
    DepSource source;
    RuntimeFetchIdentityDepKey fetchIdentity;
    RuntimeRootNarHash narHash;
    RuntimeRootStorePath storePath;
};

struct VerifiedRuntimeRootRecord {
    RuntimeRootRecord record;
    SourcePath rootPath;
};

struct RuntimeRootLoadResult {
    size_t storedCount = 0;
    size_t rejectedCount = 0;
    std::vector<RuntimeRootRecord> entries;
};

/// Read-only snapshot of a cached evaluation record, used by
/// diagnostic/inspection commands (e.g. `nix eval-info`).
struct EvalInfoRecord {
    enum class Source {
        /// Row came from the current session key (`Sessions` table).
        Session,
        /// Row came from the most recent History entry under the current
        /// stable recovery key (opt-in via `allowHistoryFallback`).
        History,
    };
    TraceId traceId;
    ResultId resultId;
    TraceHash traceHash;
    DepKeySetHash keySetHash;
    DepKeySetId depKeySetId;
    CachedResult value;
    std::shared_ptr<const std::vector<Dep>> deps;
    Source source;
};

/// Resolved dependency with owned strings. Used by verification code
/// (computeCurrentHash, resolveDepPath) and test assertions that need
/// human-readable source/key values.
struct ResolvedDep {
    struct StructuredKey {
        std::string filePath;
        StructuredFormat format;
        StructuredPath dataPath;
        ShapeSuffix suffix = ShapeSuffix::None;
        std::string hasKey;
        std::string dirSetHash;

        bool operator==(const StructuredKey &) const = default;
    };

    struct TraceContextKey {
        AttrPathId pathId;

        bool operator==(const TraceContextKey &) const = default;
    };

    std::string source;
    std::string key;
    DepHashValue expectedHash;
    CanonicalQueryKind type;
    std::optional<StructuredKey> structured;
    std::optional<TraceContextKey> traceContext;

    bool operator==(const ResolvedDep &) const = default;
};

} // namespace nix::eval_trace
