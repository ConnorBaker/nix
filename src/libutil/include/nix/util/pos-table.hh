#pragma once
///@file

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "nix/util/lru-cache.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/position.hh"
#include "nix/util/sync.hh"

namespace nix {

class PosTable
{
public:
    class Origin
    {
        friend PosTable;
    private:
        Origin(Pos::Origin origin, uint32_t offset, size_t size)
            : offset(offset)
            , origin(std::move(origin))
            , size(size)
        {
        }

    public:
        uint32_t offset;
        Pos::Origin origin;
        size_t size;

        uint32_t offsetOf(PosIdx p) const
        {
            return (p.id & ~PosIdx::tracedDataTag) - 1 - offset;
        }
    };

    /**
     * Lightweight handle returned by addOrigin(). Contains only the
     * scalars needed by add() — avoids copying the Pos::Origin variant.
     */
    struct OriginHandle
    {
        uint32_t offset;
        size_t size;
        bool tracedData;
    };

private:
    /**
     * Vector of byte offsets (in the virtual input buffer) of initial line character's position.
     * Sorted by construction. Binary search over it allows for efficient translation of arbitrary
     * byte offsets in the virtual input buffer to its line + column position.
     */
    using Lines = std::vector<uint32_t>;
    /**
     * Cache from byte offset in the virtual buffer of Origins -> @ref Lines in that origin.
     */
    using LinesCache = LRUCache<uint32_t, Lines>;

    Sync<std::map<uint32_t, Origin>> origins_;

    mutable Sync<LinesCache> linesCache;

    const Origin * resolve(PosIdx p) const
    {
        if (p.id == 0)
            return nullptr;

        const auto idx = (p.id & ~PosIdx::tracedDataTag) - 1;
        /* we want the last key <= idx, so we'll take prev(first key > idx).
            this is guaranteed to never rewind origin.begin because the first
            key is always 0. */
        auto origins(origins_.readLock());
        const auto pastOrigin = origins->upper_bound(idx);
        return &std::prev(pastOrigin)->second;
    }

public:
    PosTable(std::size_t linesCacheCapacity = 65536)
        : linesCache(linesCacheCapacity)
    {
    }

    Origin addOrigin(Pos::Origin origin, size_t size)
    {
        auto origins(origins_.lock());
        uint32_t offset = 0;
        if (auto it = origins->rbegin(); it != origins->rend())
            offset = it->first + std::max(it->second.size, static_cast<size_t>(1));
        // +1 because all PosIdx are offset by 1 to begin with, and
        // another +1 to ensure that all origins can point to EOF, eg
        // on (invalid) empty inputs.
        if (2 + offset + size < offset)
            return Origin{origin, offset, 0};
        auto [it, inserted] = origins->emplace(offset, Origin{origin, offset, size});
        assert(inserted && "PosTable origin offset collision");
        return it->second;
    }

    /**
     * Add an origin and return a lightweight handle (no variant copy).
     * Use with the OriginHandle overload of add().
     */
    OriginHandle addOriginHandle(Pos::Origin origin, size_t size)
    {
        bool td = std::holds_alternative<Pos::ProvenanceRef>(origin);
        auto origins(origins_.lock());
        uint32_t offset = 0;
        if (auto it = origins->rbegin(); it != origins->rend())
            offset = it->first + std::max(it->second.size, static_cast<size_t>(1));
        if (2 + offset + size < offset) {
            auto [it, inserted] = origins->emplace(offset, Origin{std::move(origin), offset, 0});
            assert(inserted && "PosTable origin offset collision");
            return {offset, 0, td};
        }
        auto [it, inserted] = origins->emplace(offset, Origin{std::move(origin), offset, size});
        assert(inserted && "PosTable origin offset collision");
        return {offset, size, td};
    }

    PosIdx add(const Origin & origin, size_t offset)
    {
        if (offset > origin.size)
            return PosIdx();
        uint32_t id = 1 + origin.offset + offset;
        if (std::holds_alternative<Pos::ProvenanceRef>(origin.origin))
            id |= PosIdx::tracedDataTag;
        return PosIdx(id);
    }

    PosIdx add(const OriginHandle & origin, size_t offset)
    {
        if (offset > origin.size)
            return PosIdx();
        uint32_t id = 1 + origin.offset + offset;
        if (origin.tracedData)
            id |= PosIdx::tracedDataTag;
        return PosIdx(id);
    }

    /**
     * Convert a byte-offset PosIdx into a Pos with line/column information.
     *
     * @param p Byte offset into the virtual concatenation of all parsed contents
     * @return Position
     *
     * @warning Very expensive to call, as this has to read the entire source
     * into memory each time. Call this only if absolutely necessary. Prefer
     * to keep PosIdx around instead of needlessly converting it into Pos by
     * using this lookup method.
     */
    Pos operator[](PosIdx p) const;

    Pos::Origin originOf(PosIdx p) const
    {
        if (auto o = resolve(p))
            return o->origin;
        return std::monostate{};
    }

    /** Return a pointer to the origin for p, or nullptr. No copies. */
    const Pos::Origin * originOfPtr(PosIdx p) const
    {
        if (auto o = resolve(p))
            return &o->origin;
        return nullptr;
    }

    /** Return the origin offset for p, or nullopt. Stable across vector growth. */
    std::optional<uint32_t> originOffsetOf(PosIdx p) const
    {
        if (auto o = resolve(p))
            return o->offset;
        return std::nullopt;
    }

    /** Combined origin + offset resolution in a single binary search. */
    struct ResolvedOrigin {
        const Pos::Origin * origin;
        uint32_t offset;
    };

    std::optional<ResolvedOrigin> resolveOriginFull(PosIdx p) const
    {
        if (auto o = resolve(p))
            return ResolvedOrigin{&o->origin, o->offset};
        return std::nullopt;
    }

    /**
     * Remove all origins from the table.
     */
    void clear()
    {
        linesCache.lock()->clear();
        origins_.lock()->clear();
    }
};

} // namespace nix
