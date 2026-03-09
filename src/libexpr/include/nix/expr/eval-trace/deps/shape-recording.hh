#pragma once
///@file
/// Shape dep recording: StructuredContent deps (#len, #keys, #has:key, #type),
/// TracedContainerProvenance, and compact interned dep key construction.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

namespace nix {

struct InterningPools;
struct Value;
class PosTable;

/**
 * Provenance information for a container Value (attrset or list) produced
 * by ExprTracedData::eval(). Uses interned IDs matching the session pools.
 * Used by shape-observing builtins (length, attrNames, hasAttr) to record
 * shape deps (#len, #keys, #has:key).
 */
struct TracedContainerProvenance {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * Non-owning pointer to a GC-stable TracedContainerProvenance.
 * Used for list provenance tracking (lists still use the container map).
 */
using ProvenanceRef = const TracedContainerProvenance *;

// ═══════════════════════════════════════════════════════════════════════
// Component interning -- zero-allocation dep key construction
// ═══════════════════════════════════════════════════════════════════════

/**
 * Compact interned representation of a StructuredContent dep key.
 * All fields are process-lifetime pool indices. Zero string allocation.
 * JSON dep key is constructed only for non-duplicate deps at serialization time.
 */
struct CompactDepComponents {
    DepSourceId sourceId;
    FilePathId filePathId;
    StructuredFormat format;
    DataPathId dataPathId;
    ShapeSuffix suffix;     ///< None/Len/Keys/Type
    Symbol hasKey;           ///< non-zero for #has: deps
};

/**
 * Convert a DataPath trie node to a JSON array string of path components.
 * Object keys become JSON strings; array indices become JSON numbers.
 * Only called for non-duplicate deps at serialization time.
 * Returns the serialized JSON array, e.g. '["nodes","nixpkgs",0]'.
 */
std::string dataPathToJsonString(InterningPools & pools, DataPathId nodeId);

/**
 * Convert a JSON array string of path components back to a DataPath trie node ID.
 * Used when replaying cached origins (trace-cache.cc).
 */
DataPathId jsonStringToDataPathId(InterningPools & pools, std::string_view jsonStr);

/**
 * Record a StructuredContent dep with zero-allocation dedup.
 * Hashes the compact integer components for dedup; only builds JSON
 * dep key string for non-duplicates (confirmed novel deps).
 * Returns true if the dep was recorded (not a duplicate).
 */
[[gnu::cold]] bool recordStructuredDep(
    InterningPools & pools,
    const CompactDepComponents & c,
    const DepHashValue & hash,
    DepType depType = DepType::StructuredContent);

/**
 * Precomputed keys hash from ExprTracedData::eval() Object case.
 * Stored in a thread-local side map keyed by PosTable origin offset (stable
 * from PosTable origins vector). At access time, maybeRecordAttrKeysDep compares
 * visible key count to stored count; if equal, uses the precomputed hash directly,
 * avoiding the sort + concat + BLAKE3 hash that dominates its runtime.
 * Uses interned IDs to avoid string allocation.
 */
struct PrecomputedKeysInfo {
    Blake3Hash hash;
    uint32_t keyCount;
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};


/**
 * Record a #len StructuredContent dep if the list value came from
 * ExprTracedData (checked via traced container provenance map).
 * No-op if dep tracking is inactive or list is empty (no stable key).
 */
[[gnu::cold]] void maybeRecordListLenDep(const Value & v);

/**
 * Record a #keys StructuredContent dep if the attrset contains attrs
 * with TracedData provenance (checked via PosTable::originOf on Attr::pos).
 * Groups by origin -- mixed-provenance attrsets get partial recording.
 * No-op if dep tracking is inactive or value is not an attrset.
 */
[[gnu::cold]] void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v);

/**
 * Record per-key #has deps for intersectAttrs in bulk. Pre-computes TracedData
 * origins for each operand (one scan each), then iterates keys recording:
 * - exists=true for intersection keys (tag bit check per attr, no origin scan)
 * - exists=false for disjoint keys (against pre-computed origins only)
 * Skips operands with no TracedData entirely, reducing ~100K deps to ~55
 * in the typical callPackage pattern (50-key functionArgs vs 50K allPackages).
 */
[[gnu::cold]] void recordIntersectAttrsDeps(const PosTable & positions, const SymbolTable & symbols,
                                            const Value & left, const Value & right);

/**
 * Record a #has:key StructuredContent dep using PosIdx-based provenance.
 * For exists=true: checks the found attr's PosIdx origin -- if TracedData,
 * records depHash("1"); if Nix-added (no TracedData origin), skips.
 * For exists=false: scans all attrs, records depHash("0") against each
 * unique TracedData origin.
 */
[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists);

/**
 * Record a #type StructuredContent dep if the value is a container.
 * For attrsets: uses PosIdx-based TracedData origin scanning.
 * For lists: uses the existing tracedContainerMap lookup.
 * No-op if dep tracking is inactive or value is not a container.
 */
[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v);

} // namespace nix
