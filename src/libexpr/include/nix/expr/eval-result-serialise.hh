#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/expr/file-load-tracker.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/store/path.hh"
#include "nix/util/hash.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nix {
class StoreDirConfig;
}

namespace nix::eval_cache {

/**
 * Serialize an AttrValue to CBOR bytes for storage at an output path.
 *
 * Format: CBOR map with keys:
 *   "v" (int): format version (1)
 *   "t" (int): AttrType enum value
 *   "s" (string): value string (type-dependent, empty if N/A)
 *   "c" (string): context (space-separated, String type only, empty otherwise)
 *   "n" (array of string): child names (FullAttrs only, null otherwise)
 *   "l" (int or null): list size (List only, null otherwise)
 */
std::vector<uint8_t> serializeAttrValue(const AttrValue & value, SymbolTable & symbols);

/**
 * Deserialize CBOR bytes back to an AttrValue.
 *
 * @param cbor Raw CBOR bytes read from output path.
 * @param symbols Symbol table for interning attr names.
 * @return The deserialized AttrValue.
 */
AttrValue deserializeAttrValue(const std::vector<uint8_t> & cbor, SymbolTable & symbols);

/**
 * Content-addressed eval trace: a slim result blob referencing a separate
 * dep set blob. Both are stored as Text CA objects in the Nix store.
 *
 * Result trace (~200-500 bytes): CBOR with result + parent + context + dep set path.
 * Dep set blob (zstd-compressed CBOR): standalone, independently deduped.
 */
struct EvalTrace {
    /** Cached evaluation result. */
    AttrValue result;
    /** Parent trace store path (nullopt for root). */
    std::optional<StorePath> parent;
    /** Context hash (set on root traces only). */
    std::optional<int64_t> contextHash;
    /** Store path of the dep set blob (always populated). */
    StorePath depSetPath;
};

// ── Dep set blob serialization ──────────────────────────────────────

/**
 * Serialize sorted deps to a zstd-compressed CBOR blob.
 *
 * Format: zstd(cbor([{t, s, k, h}, ...]))
 * Input deps must be pre-sorted and deduped.
 * Uses a fixed compression level for CAS determinism.
 */
std::vector<uint8_t> serializeDepSet(const std::vector<Dep> & sortedDeps);

/**
 * Deserialize a zstd-compressed CBOR dep set blob back to deps.
 */
std::vector<Dep> deserializeDepSet(const std::vector<uint8_t> & compressed);

// ── Result trace serialization (v2) ─────────────────────────────────

/**
 * Serialize an eval result trace to CBOR bytes (v2 format).
 *
 * Format: CBOR map with keys:
 *   "v" (int): trace format version (2)
 *   "r" (bytes): nested CBOR-encoded AttrValue
 *   "p" (text or null): parent trace store path string
 *   "c" (int or null): context hash (root only)
 *   "ds" (text): dep set blob store path
 *
 * Deps are NOT inlined — they live in the separate dep set blob.
 */
std::vector<uint8_t> serializeEvalTrace(
    const AttrValue & result,
    const std::optional<StorePath> & parent,
    const std::optional<int64_t> & contextHash,
    const StorePath & depSetPath,
    SymbolTable & symbols,
    const StoreDirConfig & store);

/**
 * Deserialize CBOR bytes back to an EvalTrace (v2 only).
 */
EvalTrace deserializeEvalTrace(
    const std::vector<uint8_t> & cbor,
    SymbolTable & symbols,
    const StoreDirConfig & store);

/**
 * Compute a content hash of sorted+deduped deps (without result/parent/context).
 *
 * Used by the dep hash recovery index: coldStore records this hash,
 * recovery recomputes it from current dep hashes to find matching traces.
 * Two dep sets with identical (type, source, key, hash) entries produce
 * the same content hash regardless of input order or duplicates.
 */
Hash computeDepContentHash(const std::vector<Dep> & deps);

/**
 * Compute a structural hash of deps (type, source, key only — no expectedHash).
 * Two dep sets with the same keys but different values produce the same struct hash.
 * Used for struct-group recovery (Phase 3).
 */
Hash computeDepStructHash(const std::vector<Dep> & deps);

/**
 * Compute a dep content hash that includes parent identity.
 * Appends "P" + printStorePath(parent) to the hash input.
 * Used for parent-aware recovery (Phase 2).
 */
Hash computeDepContentHashWithParent(
    const std::vector<Dep> & deps,
    const StorePath & parent,
    const StoreDirConfig & store);

/**
 * Sort deps by (type, source, key) and deduplicate by the same triple.
 * Deterministic output regardless of dep collection order.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

/**
 * Pre-sorted variants: skip internal sort+dedup (caller must provide sorted deps).
 * Used by coldStore() which sorts once and calls multiple hash/serialize functions.
 */
Hash computeDepContentHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeDepStructHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeDepContentHashWithParentFromSorted(
    const std::vector<Dep> & sortedDeps,
    const StorePath & parent,
    const StoreDirConfig & store);

} // namespace nix::eval_cache
