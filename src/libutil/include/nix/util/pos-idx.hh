#pragma once
///@file

#include <cinttypes>
#include <functional>

namespace nix {

class PosIdx
{
    friend struct LazyPosAccessors;
    friend class PosTable;
    friend class std::hash<PosIdx>;

private:
    uint32_t id;

    explicit PosIdx(uint32_t id)
        : id(id)
    {
    }

public:
    /// Tag bit set on PosIdx values from TracedData origins (O(1) provenance check).
    static constexpr uint32_t tracedDataTag = uint32_t(1) << 31;

    PosIdx()
        : id(0)
    {
    }

    /// O(1) check: does this PosIdx belong to a TracedData origin?
    bool isTracedData() const
    {
        return id & tracedDataTag;
    }

    explicit operator bool() const
    {
        return id > 0;
    }

    auto operator<=>(const PosIdx other) const
    {
        return id <=> other.id;
    }

    bool operator==(const PosIdx other) const
    {
        return id == other.id;
    }

    size_t hash() const noexcept
    {
        return std::hash<uint32_t>{}(id);
    }
};

inline PosIdx noPos = {};

} // namespace nix

namespace std {

template<>
struct hash<nix::PosIdx>
{
    std::size_t operator()(nix::PosIdx pos) const noexcept
    {
        return pos.hash();
    }
};

} // namespace std
