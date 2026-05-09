#pragma once
///@file

#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nix {

/**
 * A k=2 bloom filter for pointer membership testing.
 *
 * @tparam Bits Number of bits in the filter. Must be a power of two.
 * @tparam PointerAlignment Minimum alignment (in bytes) of stored pointers.
 *   Used to strip known-zero low bits for better hash entropy.
 *   E.g., 16 for Boehm GC-allocated Values (log2(16) = 4 bits stripped).
 *
 * Two independent hash functions produce two bit positions per pointer:
 *
 *   h1 = ptr >> log2(PointerAlignment)
 *     Strips the low bits that are always zero due to alignment,
 *     yielding the significant address bits directly.
 *
 *   h2 = (ptr >> log2(PointerAlignment)) * 0x9E3779B97F4A7C15 >> (64 - log2(Bits))
 *     Multiplicative hash using floor(2^64 / phi), where phi is the
 *     golden ratio. This classic constant spreads input bits across
 *     the full 64-bit product. The right shift extracts the top
 *     log2(Bits) bits, giving a bit position independent from h1
 *     that covers the full filter range.
 */
template <size_t Bits, size_t PointerAlignment>
struct PointerBloomFilter {
    static_assert(std::has_single_bit(Bits), "Bits must be a power of two");
    static_assert(std::has_single_bit(PointerAlignment), "PointerAlignment must be a power of two");

    static constexpr size_t SHIFT = std::countr_zero(PointerAlignment);

    /**
     * Golden ratio constant: floor(2^64 / phi).
     * Multiplicative hashing with this constant produces a second bit
     * position with good independence from the simple shift hash.
     */
    static constexpr uint64_t PHI64 = 0x9E3779B97F4A7C15ULL;

    /// Right shift to extract the top log2(Bits) bits after multiplicative hash.
    static constexpr size_t H2_SHIFT = 64 - std::countr_zero(Bits);

    /**
     * Heap-allocated to keep the filter out of GC-scanned memory.
     * Only the unique_ptr (8 bytes) is inline; the bitset itself is
     * allocated via operator new (i.e., malloc, not GC_MALLOC).
     */
    std::unique_ptr<std::bitset<Bits>> data{new std::bitset<Bits>()};

    void set(const void * v) {
        auto h = reinterpret_cast<uintptr_t>(v) >> SHIFT;
        data->set(h % Bits);
        data->set((h * PHI64 >> H2_SHIFT) % Bits);
    }

    bool test(const void * v) const {
        auto h = reinterpret_cast<uintptr_t>(v) >> SHIFT;
        return (*data)[h % Bits]
            && (*data)[(h * PHI64 >> H2_SHIFT) % Bits];
    }

    void reset() { data->reset(); }
};

} // namespace nix
