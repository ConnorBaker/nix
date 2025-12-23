#pragma once

/**
 * BigInt Encoding for HVM4
 *
 * Nix uses 64-bit signed integers, but HVM4 only has 32-bit unsigned numbers.
 * This module provides encoding/decoding between the two representations.
 *
 * Encoding Strategy:
 * - Small integers (fits in signed 32-bit): Use native NUM for efficiency
 * - Large integers: Use constructors #Pos{lo, hi} or #Neg{lo, hi}
 *
 * The small integer optimization is critical because most Nix code uses
 * values that fit in 32 bits.
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include <cstdint>
#include <optional>
#include <limits>

namespace nix::hvm4 {

// Constructor tags for BigInt encoding
// These are unique identifiers used in the EXT field.
// IMPORTANT: These MUST be large values to avoid collision with NUM values!
// HVM4's MAT instruction compares ext(mat) with val(num) for NUM terms,
// so if we use small values like 1 or 2, small integers will incorrectly match.
// We use values > 0x100000 (1 million+) to avoid collision with typical integers.
constexpr uint32_t BIGINT_POS = 0x100001;  // Positive BigInt: value = (hi << 32) | lo
constexpr uint32_t BIGINT_NEG = 0x100002;  // Negative BigInt: value = -((hi << 32) | lo)

/**
 * Check if a 64-bit signed value fits in a 32-bit signed range.
 *
 * When it fits, we can use the native NUM representation with the
 * bit pattern interpreted as signed.
 */
inline bool fitsInSmallInt(int64_t value) {
    return value >= std::numeric_limits<int32_t>::min()
        && value <= std::numeric_limits<int32_t>::max();
}

/**
 * Encode a 64-bit signed integer as an HVM4 term.
 *
 * For small values (fits in int32_t), uses native NUM.
 * For large values, uses constructor encoding.
 *
 * @param value The value to encode
 * @param runtime The HVM4 runtime (for heap allocation)
 * @return The encoded term
 */
Term encodeInt64(int64_t value, HVM4Runtime& runtime);

/**
 * Decode an HVM4 term to a 64-bit signed integer.
 *
 * @param term The term to decode (must be in normal form)
 * @param runtime The HVM4 runtime (for heap access)
 * @return The decoded value, or nullopt if not a valid integer encoding
 */
std::optional<int64_t> decodeInt64(Term term, const HVM4Runtime& runtime);

/**
 * Check if a term represents a BigInt (NUM or constructor encoding).
 */
bool isBigInt(Term term);

/**
 * Emit HVM4 code for BigInt addition.
 *
 * For small integers, this just uses OP2(OP_ADD).
 * For BigInts, this needs to handle overflow detection and propagation.
 *
 * Note: For the initial prototype, we only support small integer addition.
 */
Term emitBigIntAdd(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for BigInt comparison (equality).
 *
 * For small integers, uses OP2(OP_EQ).
 * For BigInts, compares both components.
 */
Term emitBigIntEq(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for signed less-than comparison.
 *
 * HVM4's OP_LT treats values as unsigned, but Nix uses signed integers.
 * This function emits code that correctly handles signed comparison by
 * XORing the sign bit before unsigned comparison:
 *   signed_lt(a, b) = unsigned_lt(a XOR 0x80000000, b XOR 0x80000000)
 *
 * For small integers (in 32-bit range), this works correctly.
 * For BigInts, the sign is encoded in the constructor tag, so we need
 * special handling.
 */
Term emitSignedLessThan(Term a, Term b, HVM4Runtime& runtime);

// Constructor tag for null value (arity-0 constructor)
// Using a distinct tag from BigInt to represent null
// IMPORTANT: Must be large to avoid collision with NUM values (see BIGINT_POS comment)
constexpr uint32_t NIX_NULL = 0x100003;

// Constructor tag for float values
// Floats are stored as #Flt{lo, hi} where lo and hi are the lower and upper
// 32 bits of the IEEE 754 double representation.
constexpr uint32_t NIX_FLT = 0x100004;

/**
 * Create a null term.
 *
 * We use a constructor #Nul{} instead of ERA because ERA has special
 * semantics in HVM4 (any operation involving ERA returns ERA).
 * Using a constructor allows null comparisons to work correctly.
 */
Term makeNull(HVM4Runtime& runtime);

/**
 * Check if a term represents null.
 */
bool isNull(Term term);

/**
 * Emit HVM4 code for null-aware equality comparison.
 *
 * Handles the special case where one or both operands might be null:
 * - null == null -> 1 (true)
 * - null == x -> 0 (false) for any non-null x
 * - x == y -> normal comparison
 */
Term emitNullAwareEq(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for null-aware inequality comparison.
 */
Term emitNullAwareNEq(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for BigInt-aware less-than comparison.
 *
 * Handles all cases of integer comparison:
 * - NUM vs NUM: signed comparison using XOR trick
 * - #Neg{} vs anything: #Neg{} is always smaller (except vs other #Neg{})
 * - #Pos{} vs anything: #Pos{} is always larger (except vs other #Pos{})
 * - #Pos{} vs #Pos{}: compare hi, then lo (unsigned)
 * - #Neg{} vs #Neg{}: compare hi, then lo (reversed - larger magnitude = smaller)
 *
 * Ordering: #Neg{...} < NUM(-2^31..-1) < NUM(0..2^31-1) < #Pos{...}
 */
Term emitBigIntLessThan(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for BigInt-aware equality comparison.
 *
 * Handles all cases:
 * - NUM vs NUM: direct comparison
 * - #Pos{} vs #Pos{}: compare lo and hi
 * - #Neg{} vs #Neg{}: compare lo and hi
 * - Mixed types: always false
 */
Term emitBigIntEquality(Term a, Term b, HVM4Runtime& runtime);

/**
 * Emit HVM4 code for BigInt-aware inequality comparison.
 */
Term emitBigIntInequality(Term a, Term b, HVM4Runtime& runtime);

/**
 * Simplified BigInt-aware less-than (for debugging).
 * Only handles NUM vs NUM correctly, used to verify MAT structure works.
 */
Term emitBigIntLessThanSimple(Term a, Term b, HVM4Runtime& runtime);

// ============================================================================
// Float Encoding
// ============================================================================

/**
 * Encode a double as an HVM4 term.
 *
 * Floats are stored as #Flt{lo, hi} where lo and hi are the lower and upper
 * 32 bits of the IEEE 754 double representation.
 *
 * @param value The double value to encode
 * @param runtime The HVM4 runtime (for heap allocation)
 * @return The encoded term
 */
Term encodeFloat(double value, HVM4Runtime& runtime);

/**
 * Decode an HVM4 term to a double.
 *
 * @param term The term to decode (must be in normal form)
 * @param runtime The HVM4 runtime (for heap access)
 * @return The decoded value, or nullopt if not a valid float encoding
 */
std::optional<double> decodeFloat(Term term, const HVM4Runtime& runtime);

/**
 * Check if a term represents a float (#Flt{lo, hi} constructor).
 */
bool isFloat(Term term);

}  // namespace nix::hvm4
