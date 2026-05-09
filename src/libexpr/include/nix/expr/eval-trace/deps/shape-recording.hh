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
 * The key is serialized directly into the typed DepKeySets payload; no JSON
 * dep-key string is constructed on the hot recording path.
 */
struct CompactDepComponents {
    DepSourceId sourceId;
    FilePathId filePathId;
    StructuredFormat format;
    DataPathId dataPathId;
    ShapeSuffix suffix;     ///< None/Len/Keys/Type
    StringId hasKeyId;       ///< non-zero for #has: deps
    StringId dirSetHashId;   ///< non-zero for aggregated DirSet deps
};

/**
 * Resolve an interned DataPath trie node to a typed structured path.
 */
StructuredPath resolveStructuredPath(const InterningPools & pools, DataPathId nodeId);

/**
 * Intern a typed structured path back into the session DataPath trie.
 */
DataPathId internStructuredPath(InterningPools & pools, const StructuredPath & path);

/**
 * Record a StructuredContent dep with zero-allocation dedup.
 * Hashes the compact integer components for dedup and records a typed dep key.
 * Returns true if the dep was recorded (not a duplicate).
 */
[[gnu::cold]] bool recordStructuredDep(
    InterningPools & pools,
    const CompactDepComponents & c,
    const DepHashValue & hash,
    CanonicalQueryKind depType = CanonicalQueryKind::StructuredProjection);

/**
 * Precomputed keys hash from ExprTracedData::eval() Object case.
 * Stored in a thread-local side map keyed by PosTable origin offset (stable
 * from PosTable origins vector). At access time, maybeRecordAttrKeysDep compares
 * visible key count to stored count; if equal, uses the precomputed hash directly,
 * avoiding the sort + hash that dominates its runtime.
 * Uses interned IDs to avoid string allocation.
 */
struct PrecomputedKeysInfo {
    DepHash hash{};
    uint32_t keyCount = 0;
    DepSourceId sourceId{};
    FilePathId filePathId{};
    DataPathId dataPathId{};
    StructuredFormat format{};
};


/**
 * Record a #len StructuredContent dep if the list value came from
 * ExprTracedData (checked via the TraceFrame container-provenance cache).
 * No-op if dep tracking is inactive or the list has no registered stable
 * provenance key. Traced and derived lists use heap-backed list storage so
 * their shape remains observable without reusing a stale Value address or
 * conflating independently-built lists that contain the same elements.
 *
 * SUBSUMPTION DEPENDENCY: When this dep is recorded on a derived-container
 * output (e.g., a filtered list from DerivedContainerBuilder), the stored
 * hash is hash(output.len), not hash(source.len).  Verification correctness
 * relies on the subsumption short-circuit in resolveDepHash<CurrentTraceDep>:
 * when the source file is unchanged, the stored hash is returned directly
 * without recomputing from the source.  Without subsumption, this dep would
 * fail verification for any filter output whose length differs from the
 * source.
 *
 * RECOVERY: tryStructuralVariantRecovery iterates with CandidateDep and
 * the representative key set (loadKeySet).  CandidateDep does
 * NOT take the subsumption shortcut — resolveDepHash runs the compute path
 * for every dep, producing hash(op(current F)).  Recovery succeeds iff the
 * structural operation applied to the current source file reproduces the
 * candidate's recorded output hash.  VerifiedHash L1 writes from
 * CandidateDep are structurally unreachable (the `VerifiedSubsumption`
 * factory is SFINAE-restricted to CurrentTrace), so historical stored
 * hashes cannot poison L1.
 *
 * See DerivedContainerBuilder class comment for the full analysis.
 */
[[gnu::cold]] void maybeRecordListLenDep(const Value & v);

/**
 * Record a #keys StructuredContent dep if the attrset contains attrs
 * with TracedData provenance (checked via PosTable::originOf on Attr::pos).
 * Groups by origin -- mixed-provenance attrsets get partial recording.
 * No-op if dep tracking is inactive or value is not an attrset.
 *
 * SUBSUMPTION DEPENDENCY: When this dep is recorded on a derived-container
 * output (e.g., a removeAttrs/intersectAttrs result from DerivedContainerBuilder),
 * the stored hash is hash(output.keys), not hash(source.keys).  Verification
 * correctness relies on the subsumption short-circuit in
 * resolveDepHash<CurrentTraceDep>: when the source file is unchanged, the
 * stored hash is returned directly without recomputing from the source.
 * Without subsumption, this dep would fail verification for any
 * derived-attrset output whose key set differs from the source.
 *
 * RECOVERY: same as maybeRecordListLenDep — tryStructuralVariantRecovery
 * iterates with CandidateDep and loadKeySet.  CandidateDep always takes
 * the compute path (subsumption shortcut is compiled only for CurrentTrace),
 * so recovery reproduces hash(op(current F)) from scratch and matches only
 * when current F yields the candidate's recorded output shape.  See
 * DerivedContainerBuilder class comment for the full analysis.
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
 * For lists: uses the TraceFrame container-provenance lookup.
 * No-op if dep tracking is inactive or value is not a container.
 */
[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v);

} // namespace nix
