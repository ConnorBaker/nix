#pragma once
///@file
/// TraceFrame: Lifetime 2 (per root-tracker) cache storage and lookup arena.
/// Owned by recording.cc root-frame lifecycle code; helper-facing operations go
/// through TraceAccess rather than a general-purpose frame API.

#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-gc.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nix {

class Bindings;
struct Value;
namespace eval_trace {
struct TraceAccess;
struct FiberTraceFrameScope;
struct FiberEvalContext;
struct DepCaptureScope;
}

struct TransparentStringHash {
    using is_avalanching = void;
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    size_t operator()(const std::string & value) const noexcept
    {
        return (*this)(std::string_view(value));
    }

    size_t operator()(const char * value) const noexcept
    {
        return (*this)(std::string_view(value));
    }
};

struct DirSetKey {
    std::vector<std::pair<DepSourceId, FilePathId>> sorted;
    bool operator==(const DirSetKey &) const = default;
    struct Hash {
        size_t operator()(const DirSetKey & k) const noexcept {
            size_t seed = k.sorted.size();
            for (auto & [s, f] : k.sorted)
                hash_combine(seed, s.value, f.value);
            return seed;
        }
    };
};

struct IntersectOriginInfo {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * Container for all Lifetime 2 cache storage. Created when a root
 * DepRecordingContext is constructed (depth 0->1), destroyed when the root
 * frame is destroyed (depth 1->0). Adding a new L2 cache is just adding a
 * field.
 *
 * Operational access is intentionally centralized in TraceAccess's
 * implementation so helper-facing code does not depend on a general-purpose
 * concrete root-scope surface. Between root tracker scopes, there is no scope.
 */
struct TraceFrame {
    // Typed registries for the root-scope lifetime-2 payload. Raw pointer
    // identity stays internal to the registry key wrappers rather than
    // leaking through the frame surface. TraceAccess remains the sole
    // runtime-facing access point.
private:
    friend struct TraceFrameScope;
    friend struct eval_trace::FiberTraceFrameScope;
    friend struct eval_trace::FiberEvalContext;
    friend struct eval_trace::DepCaptureScope;
    friend struct eval_trace::TraceAccess;
    TraceFrame() = default;

    static TraceFrame * swapCurrent(TraceFrame * next);
    static TraceFrame * currentForAccess();

    /// Lookup key for the container provenance registry.
    ///
    /// For **attrsets**, the key is the heap-stable `Bindings*` identity.
    ///
    /// For **lists**, the key is a heap-stable list-storage pointer. Inline
    /// small lists and default empty lists have no stable provenance key.
    /// Lookups on those values return no provenance; registration is an
    /// internal invariant violation because it would silently lose precision.
    /// Traced or derived lists that need provenance must be built with heap
    /// storage.
    enum class ContainerRefKind : uint8_t {
        Attrs,
        List,
    };

    struct ContainerRef {
        ContainerRefKind kind;
        const void * value;
        bool operator==(const ContainerRef &) const = default;
        struct Hash {
            size_t operator()(const ContainerRef & key) const noexcept
            {
                size_t seed = std::hash<uint8_t>{}(static_cast<uint8_t>(key.kind));
                seed ^= std::hash<const void *>{}(key.value)
                    + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };
    };

    struct BindingsRef {
        const Bindings * value;
        bool operator==(const BindingsRef &) const = default;
        struct Hash {
            size_t operator()(const BindingsRef & key) const noexcept
            {
                return std::hash<const Bindings *>{}(key.value);
            }
        };
    };

    /// ContainerProvenanceRegistry: authoritative provenance for traced
    /// containers whose shape may be observed after transformations.
    ///
    /// List provenance (StructuredObject / TracedContainerProvenance) CANNOT be
    /// stored inline in a Value on x86-64, because the packed ValueStorage
    /// specialization fits the entire Value into 128 bits (two 64-bit words):
    /// one word for the elems pointer (with pdListN discriminator bits) and one
    /// word for the size. A third pointer for provenance does not fit.
    ///
    /// This registry provides the cross-platform-uniform list mechanism. Lists
    /// are keyed by heap-backed list-storage identity rather than the Value*
    /// address, so bitwise-copied heap-backed Values resolve to the same entry
    /// automatically without conflating two independently-built lists that
    /// happen to contain the same element pointers.
    ///
    /// Attrsets are keyed by the heap-allocated Bindings* identity, not by the
    /// enclosing Value*, so overwriting a Value with another attrset cannot
    /// accidentally inherit old provenance. Bindings::publication_ remains the
    /// durable semantic publication channel for attrsets; this registry is the
    /// trace-frame-local shape propagation channel.
    ///
    /// The map uses traceable_allocator because its keys contain GC-allocated
    /// backing pointers (Bindings* or Value**). Keeping those pointers visible
    /// to Boehm pins each registered backing object for the TraceFrame lifetime
    /// and prevents address reuse from making a later container inherit stale
    /// provenance.
    ///
    /// This registry lives inside TraceFrame (trace-recording lifetime). It is
    /// NOT accessible outside a TraceAccess scope — do not build any mechanism
    /// that calls lookupTracedContainer outside active trace recording.
    struct ContainerProvenanceRegistry {
        // ContainerRef -> container provenance.
        boost::unordered_flat_map<
            ContainerRef,
            const TracedContainerProvenance *,
            ContainerRef::Hash,
            std::equal_to<ContainerRef>,
            traceable_allocator<std::pair<const ContainerRef, const TracedContainerProvenance *>>>
            provenanceByContainer;
        // Stable pool for TracedContainerProvenance data (deque = no pointer invalidation).
        std::deque<TracedContainerProvenance> provenancePool;

        ProvenanceRef allocate(
            DepSourceId sourceId,
            FilePathId filePathId,
            DataPathId dataPathId,
            StructuredFormat format);
        static std::optional<ContainerRef> makeRef(const Value * key);
        void registerContainer(const Value * key, const TracedContainerProvenance * prov);
        void unregisterContainer(const Value * key);
        const TracedContainerProvenance * lookupContainer(const Value * key) const;
    };

    struct ScannedBindingsRegistry {
        // Skip re-scanning the same Bindings* in maybeRecordAttrKeysDep.
        //
        // The key contains a GC-allocated Bindings*. Keep the table storage
        // traceable for the same reason as ContainerProvenanceRegistry: an
        // invisible pointer key could let Boehm reclaim/reuse the Bindings
        // address during the TraceFrame lifetime, turning this memo table into
        // a stale "already scanned" answer for a different attrset.
        boost::unordered_flat_set<
            BindingsRef,
            BindingsRef::Hash,
            std::equal_to<BindingsRef>,
            traceable_allocator<BindingsRef>>
            scanned;

        bool markScanned(const Bindings * bindings);
        bool contains(const Bindings * bindings) const;
    };

    struct PrecomputedKeysRegistry {
        // Origin offset -> precomputed keys hash.
        boost::unordered_flat_map<uint32_t, PrecomputedKeysInfo> byOriginOffset;

        void registerKeys(uint32_t originOffset, PrecomputedKeysInfo info);
        void eraseKeys(uint32_t originOffset);
        const PrecomputedKeysInfo * lookup(uint32_t originOffset) const;
    };

    struct IntersectOriginsRegistry {
        // Bindings* -> origin info cache for intersectAttrs bulk recording.
        //
        // The key contains a GC-allocated Bindings*. The value stores only
        // interned IDs, but the table allocation itself must keep the key
        // visible to Boehm so address reuse cannot return stale origins for a
        // different attrset.
        boost::unordered_flat_map<
            BindingsRef,
            std::vector<IntersectOriginInfo>,
            BindingsRef::Hash,
            std::equal_to<BindingsRef>,
            traceable_allocator<std::pair<const BindingsRef, std::vector<IntersectOriginInfo>>>>
            byBindings;

        const std::vector<IntersectOriginInfo> * lookup(const Bindings * bindings) const;
        const std::vector<IntersectOriginInfo> * cache(const Bindings * bindings, std::vector<IntersectOriginInfo> origins);
        size_t size() const;
    };

    struct DirSetHashRegistry {
        // Sorted dir-set -> active-backend hex hash cache.
        boost::unordered_flat_map<DirSetKey, std::string, DirSetKey::Hash> byDirSet;

        std::optional<std::string_view> lookup(const DirSetKey & key) const;
        void cache(DirSetKey key, std::string hash);
        size_t size() const;
    };

    ContainerProvenanceRegistry containerProvenanceRegistry;
    ScannedBindingsRegistry scannedBindingsRegistry;
    PrecomputedKeysRegistry precomputedKeysRegistry;
    IntersectOriginsRegistry intersectOriginsRegistry;
    DirSetHashRegistry dirSetHashRegistry;

    ProvenanceRef allocateProvenance(
        DepSourceId sourceId,
        FilePathId filePathId,
        DataPathId dataPathId,
        StructuredFormat format);

    void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info);
    void erasePrecomputedKeys(uint32_t originOffset);
    void registerTracedContainer(const Value * key, const TracedContainerProvenance * prov);
    void unregisterTracedContainer(const Value * key);
    const TracedContainerProvenance * lookupTracedContainer(const Value * key) const;
    bool markBindingsScanned(const Bindings * bindings);
    bool isBindingsScannedForTest(const Bindings * bindings) const;
    const PrecomputedKeysInfo * lookupPrecomputedKeys(uint32_t originOffset) const;
    const std::vector<IntersectOriginInfo> * lookupIntersectOrigins(const Bindings * bindings) const;
    const std::vector<IntersectOriginInfo> * cacheIntersectOriginsIfScoped(
        const Bindings * bindings,
        std::vector<IntersectOriginInfo> origins);
    std::optional<std::string_view> lookupDirSetHash(const DirSetKey & key) const;
    void cacheDirSetHash(DirSetKey key, std::string hash);
    size_t dirSetHashCacheSizeForTest() const;
    size_t intersectOriginsCacheSizeForTest() const;

    TraceFrame(const TraceFrame &) = delete;
    TraceFrame & operator=(const TraceFrame &) = delete;
};

} // namespace nix
