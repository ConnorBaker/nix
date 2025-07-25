#pragma once
///@file

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <limits>

#include "nix/util/error.hh"

namespace nix {

/**
 * Provides an indexable container like vector<> with memory overhead
 * guarantees like list<> by allocating storage in chunks of ChunkSize
 * elements instead of using a contiguous memory allocation like vector<>
 * does. Not using a single vector that is resized reduces memory overhead
 * on large data sets by on average (growth factor)/2, mostly
 * eliminates copies within the vector during resizing, and provides stable
 * references to its elements.
 */
template<typename T, size_t ChunkSize>
class ChunkedVector
{
private:
    uint32_t size_ = 0;
    std::vector<std::vector<T>> chunks;

    /**
     * Keep this out of the ::add hot path
     */
    [[gnu::noinline]]
    auto & addChunk()
    {
        if (size_ >= std::numeric_limits<uint32_t>::max() - ChunkSize)
            unreachable();
        chunks.emplace_back();
        chunks.back().reserve(ChunkSize);
        return chunks.back();
    }

public:
    ChunkedVector(uint32_t reserve)
    {
        chunks.reserve(reserve);
        addChunk();
    }

    uint32_t size() const noexcept
    {
        return size_;
    }

    template<typename... Args>
    std::pair<T &, uint32_t> add(Args &&... args)
    {
        const auto idx = size_++;
        auto & chunk = [&]() -> auto & {
            if (auto & back = chunks.back(); back.size() < ChunkSize)
                return back;
            return addChunk();
        }();
        auto & result = chunk.emplace_back(std::forward<Args>(args)...);
        return {result, idx};
    }

    /**
     * Unchecked subscript operator.
     * @pre add must have been called at least idx + 1 times.
     * @throws nothing
     */
    const T & operator[](uint32_t idx) const noexcept
    {
        return chunks[idx / ChunkSize][idx % ChunkSize];
    }

    template<typename Fn>
    void forEach(Fn fn) const
    {
        for (const auto & c : chunks)
            for (const auto & e : c)
                fn(e);
    }
};
} // namespace nix
