#pragma once

#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-index-db.hh"
#include "nix/expr/eval-result-serialise.hh"
#include "nix/expr/file-load-tracker.hh"
#include "nix/store/store-api.hh"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace nix::eval_cache {

struct ExprCached;
struct EvalCacheStore;

/**
 * GC-allocated deferred cold store operation.
 *
 * Captures ExprCached* pointers that must remain visible to the
 * Boehm GC. Inherits from `gc` so that `new DeferredColdStore(...)`
 * allocates via GC_MALLOC — the GC scans this memory and finds the
 * captured ExprCached* fields, preventing premature collection.
 *
 * Without this, std::function<void()> lambdas on the malloc heap
 * capture raw ExprCached* pointers invisible to the GC, leading to
 * use-after-free when flush() runs during ~EvalCacheStore().
 */
struct DeferredColdStore : gc
{
    EvalCacheStore * sb;
    ExprCached * target;      ///< where to write tracePath after cold store
    ExprCached * parentSrc;   ///< where to read parent tracePath (null for root)
    std::string attrPath;
    std::string name;
    AttrValue value;
    std::vector<Dep> deps;
    bool isRoot;

    void execute();
};

/**
 * Human-readable dep type name for trace naming.
 * Returns e.g. "content", "directory", "existence".
 */
std::string depTypeString(DepType type);

/**
 * Store-based eval cache backend using two-object content-addressed traces.
 *
 * Each cached attribute is stored as two Text CA objects in the Nix store:
 *   1. Result trace (~200-500 bytes): CBOR with result + parent + context + dep set path.
 *   2. Dep set blob (zstd-compressed CBOR): standalone, independently deduped.
 *
 * Parent relationships are encoded via store path references, enabling
 * transitive invalidation through the store's reference graph. Result traces
 * reference both their parent trace and their dep set blob.
 */
struct EvalCacheStore
{
    Store & store;
    EvalIndexDb index;
    SymbolTable & symbols;
    int64_t contextHash;

    /**
     * Session-cached validated trace paths.
     * Once a trace and all its ancestors are validated,
     * the result is known valid for this session.
     */
    std::set<StorePath> validatedTraces;

    /**
     * Session-cached validated dep set paths.
     * Siblings sharing the same dep set blob skip redundant validation.
     */
    std::set<StorePath> validatedDepSets;

    /**
     * Session cache of decompressed dep sets, keyed by dep set store path.
     * Avoids redundant decompression when validation + replay both need deps.
     */
    std::map<StorePath, std::vector<Dep>> depSetCache;

    /**
     * Deferred cold store operations. Executed in FIFO order during flush().
     * Parent writes are always pushed before children (evaluation is depth-first),
     * so parent tracePaths are set before children read them.
     *
     * Uses traceable_allocator so the GC scans the vector storage and finds
     * the DeferredColdStore* pointers (which are themselves GC-allocated),
     * keeping the captured ExprCached* fields reachable.
     */
    std::vector<DeferredColdStore *, traceable_allocator<DeferredColdStore *>> deferredWrites;

    /**
     * Attr paths with pending deferred writes (for dedup).
     */
    std::unordered_set<std::string> stagedAttrPaths;

    bool isStaged(const std::string & attrPath) const;
    void defer(const std::string & attrPath, DeferredColdStore * write);
    void flush();
    ~EvalCacheStore();

    /**
     * Clear all session-scoped validation caches.
     * Forces subsequent validateTrace/warmPath calls to re-check deps.
     */
    void clearSessionCaches();


    EvalCacheStore(Store & store, SymbolTable & symbols, int64_t contextHash);

    // ── Cold Path ────────────────────────────────────────────────

    /**
     * Store a cold-path evaluation result as a Text CA trace blob.
     *
     * @param attrPath Null-byte-separated attr path.
     * @param name Attribute name for store path naming.
     * @param value The evaluated AttrValue.
     * @param directDeps Direct (non-inherited) deps from FileLoadTracker.
     * @param parentTracePath Parent trace store path.
     * @param isRoot Whether this is the root attribute.
     * @return tracePath (single path, used for both identification and result).
     */
    StorePath coldStore(
        std::string_view attrPath,
        std::string_view name,
        const AttrValue & value,
        const std::vector<Dep> & directDeps,
        const std::optional<StorePath> & parentTracePath,
        bool isRoot);

    // ── Warm Path ────────────────────────────────────────────────

    struct WarmResult {
        AttrValue value;
        StorePath tracePath;
    };

    /**
     * Try to serve a cached result from the warm path.
     *
     * Looks up index → validates trace deps → reads result from trace.
     *
     * @param attrPath Null-byte-separated attr path.
     * @param inputAccessors Flake input name → source path mapping.
     * @param state EvalState for dep validation.
     * @param parentTraceHint Parent's recovered trace path (for Phase 2 recovery).
     * @return WarmResult if valid, nullopt otherwise.
     */
    std::optional<WarmResult> warmPath(
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        const std::optional<StorePath> & parentTraceHint = std::nullopt);

    // ── Recovery ─────────────────────────────────────────────────

    /**
     * Try three-phase content-addressed recovery after warm-path validation failure.
     *
     * Phase 1: depHash point lookup — same dep structure, different values.
     * Phase 2: parent-aware depHash lookup — finds children by parent identity.
     * Phase 3: struct-group scan — finds traces with same dep keys but different structure.
     *
     * @param oldTracePath The invalidated trace path.
     * @param attrPath Null-byte-separated attr path.
     * @param inputAccessors Flake input name → source path mapping.
     * @param state EvalState for hash computation.
     * @param parentTraceHint Parent's recovered trace path (for Phase 2).
     * @return WarmResult if recovered, nullopt otherwise.
     */
    std::optional<WarmResult> recovery(
        const StorePath & oldTracePath,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        const std::optional<StorePath> & parentTraceHint = std::nullopt);

    // ── Trace I/O ────────────────────────────────────────────────

    /**
     * Store a CBOR blob as a Text CA object in the Nix store.
     * Returns the content-addressed store path.
     * Skips writing if the path already exists (dedup).
     */
    StorePath storeTrace(
        std::string_view name,
        const std::vector<uint8_t> & cbor,
        const StorePathSet & references);

    /**
     * Load and deserialize a result trace from the store.
     * Returns the EvalTrace (result + parent + depSetPath, no inline deps).
     */
    EvalTrace loadTrace(const StorePath & tracePath);

    // ── Dep Set I/O ─────────────────────────────────────────────

    /**
     * Store a compressed dep set blob as a Text CA object.
     * Name: "eval-deps". References: {} (standalone).
     * Returns the content-addressed store path.
     */
    StorePath storeDepSet(const std::vector<uint8_t> & compressed);

    /**
     * Load and decompress a dep set from the store.
     * Uses depSetCache to avoid redundant decompression.
     */
    std::vector<Dep> loadDepSet(const StorePath & depSetPath);

    /**
     * Load deps for a result trace (convenience: loadTrace → loadDepSet).
     */
    std::vector<Dep> loadDepsForTrace(const StorePath & tracePath);

    // ── Trace Validation ────────────────────────────────────────

    /**
     * Validate a trace: load deps from dep set blob, check all deps
     * against current state, recursively validate parent.
     */
    bool validateTrace(
        const StorePath & tracePath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    // ── Helpers ──────────────────────────────────────────────────

    /**
     * Try a recovery candidate from DepHashRecovery.
     * Validates the candidate trace and updates the index on success.
     */
    std::optional<WarmResult> tryCandidate(
        const Hash & depHash,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::set<StorePath> & tried);

    /**
     * Build the null-byte-separated attr path from components.
     */
    static std::string buildAttrPath(const std::vector<std::string> & components);

    /**
     * Sanitize an attribute name for use in a store path.
     * Replaces invalid characters with '-'.
     */
    static std::string sanitizeName(std::string_view name);
};

} // namespace nix::eval_cache
