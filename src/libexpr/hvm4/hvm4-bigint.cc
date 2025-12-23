/**
 * BigInt Encoding Implementation
 *
 * Handles encoding and decoding of 64-bit signed integers for HVM4.
 *
 * HVM4's native NUM type only supports 32-bit unsigned values. To represent
 * Nix's 64-bit signed integers, we use a dual strategy:
 *
 * 1. Small Integer Optimization: Values that fit in int32_t are stored
 *    directly as NUM terms (interpreting bits as signed).
 *
 * 2. BigInt Encoding: Larger values use constructor encoding:
 *    - #Pos{lo, hi} for positive values where value = (hi << 32) | lo
 *    - #Neg{lo, hi} for negative values where value = -((hi << 32) | lo)
 *
 * The small integer optimization is critical for performance since most
 * Nix code uses values that fit in 32 bits.
 */

#include "nix/expr/hvm4/hvm4-bigint.hh"

#include <cstring>

namespace nix::hvm4 {

Term encodeInt64(int64_t value, HVM4Runtime& runtime) {
    // Small integer optimization: use native NUM
    if (fitsInSmallInt(value)) {
        // Store the signed value as a bit pattern
        uint32_t bits = static_cast<uint32_t>(static_cast<int32_t>(value));
        return HVM4Runtime::termNewNum(bits);
    }

    // Large integer: use constructor encoding
    // We encode as #Pos{lo, hi} or #Neg{lo, hi}
    // where value = +/- ((hi << 32) | lo)

    uint64_t absVal;
    uint32_t tag;

    if (value >= 0) {
        absVal = static_cast<uint64_t>(value);
        tag = BIGINT_POS;
    } else {
        // Handle INT64_MIN carefully to avoid undefined behavior
        if (value == std::numeric_limits<int64_t>::min()) {
            absVal = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
        } else {
            absVal = static_cast<uint64_t>(-value);
        }
        tag = BIGINT_NEG;
    }

    uint32_t lo = static_cast<uint32_t>(absVal & 0xFFFFFFFF);
    uint32_t hi = static_cast<uint32_t>((absVal >> 32) & 0xFFFFFFFF);

    // Create constructor with 2 fields: #Tag{lo, hi}
    // C02 is the tag for constructor with arity 2
    Term args[2] = {
        HVM4Runtime::termNewNum(lo),
        HVM4Runtime::termNewNum(hi)
    };

    return runtime.termNewCtr(tag, 2, args);
}

std::optional<int64_t> decodeInt64(Term term, const HVM4Runtime& runtime) {
    uint8_t tag = HVM4Runtime::termTag(term);

    // Check for native NUM (small integer)
    if (tag == HVM4Runtime::TAG_NUM) {
        uint32_t bits = HVM4Runtime::termVal(term);
        // Interpret as signed 32-bit
        int32_t signedVal = static_cast<int32_t>(bits);
        return static_cast<int64_t>(signedVal);
    }

    // Check for constructor with arity 2 (BigInt encoding)
    if (tag == HVM4Runtime::TAG_C02) {
        uint32_t name = HVM4Runtime::termExt(term);
        uint32_t loc = HVM4Runtime::termVal(term);

        // Read the two fields
        Term loTerm = runtime.load(loc);
        Term hiTerm = runtime.load(loc + 1);

        // Both fields must be NUM
        if (HVM4Runtime::termTag(loTerm) != HVM4Runtime::TAG_NUM ||
            HVM4Runtime::termTag(hiTerm) != HVM4Runtime::TAG_NUM) {
            return std::nullopt;
        }

        uint32_t lo = HVM4Runtime::termVal(loTerm);
        uint32_t hi = HVM4Runtime::termVal(hiTerm);
        uint64_t absVal = (static_cast<uint64_t>(hi) << 32) | lo;

        if (name == BIGINT_POS) {
            // Check for overflow into negative range
            if (absVal > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return std::nullopt;
            }
            return static_cast<int64_t>(absVal);
        } else if (name == BIGINT_NEG) {
            // Handle INT64_MIN specially
            if (absVal == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
                return std::numeric_limits<int64_t>::min();
            }
            if (absVal > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return std::nullopt;
            }
            return -static_cast<int64_t>(absVal);
        }
    }

    // Not a valid integer encoding
    return std::nullopt;
}

bool isBigInt(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);

    // Native NUM is always a valid integer
    if (tag == HVM4Runtime::TAG_NUM) {
        return true;
    }

    // Constructor with arity 2 could be a BigInt
    if (tag == HVM4Runtime::TAG_C02) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == BIGINT_POS || name == BIGINT_NEG;
    }

    return false;
}

Term emitBigIntAdd(Term a, Term b, HVM4Runtime& runtime) {
    // For the initial prototype, we just use native OP2 addition.
    // This works correctly for small integers.
    //
    // TODO: For full BigInt support, we would need to:
    // 1. Check if either operand is a BigInt constructor
    // 2. If so, use a more complex addition that handles overflow
    // 3. For now, we assume operands are small integers
    return runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
}

Term emitBigIntEq(Term a, Term b, HVM4Runtime& runtime) {
    // For the initial prototype, we just use native OP2 equality.
    // This works correctly for small integers that use the same encoding.
    //
    // TODO: For full BigInt support, we would need to handle mixed cases
    // where one operand is NUM and the other is a BigInt constructor
    // (they could still be equal if the BigInt represents a small value).
    return runtime.termNewOp2(HVM4Runtime::OP_EQ, a, b);
}

Term emitSignedLessThan(Term a, Term b, HVM4Runtime& runtime) {
    // For signed comparison on 32-bit values stored as unsigned in HVM4,
    // we use the XOR trick: XOR with 0x80000000 flips the sign bit,
    // converting signed comparison to unsigned comparison.
    //
    // signed_lt(a, b) = unsigned_lt(a ^ 0x80000000, b ^ 0x80000000)
    //
    // This works because:
    // - Negative numbers (0x80000000-0xFFFFFFFF) become 0x00000000-0x7FFFFFFF
    // - Positive numbers (0x00000000-0x7FFFFFFF) become 0x80000000-0xFFFFFFFF
    // So after XOR, the relative order matches signed ordering.

    constexpr uint32_t SIGN_BIT = 0x80000000;
    Term signBit = HVM4Runtime::termNewNum(SIGN_BIT);

    // XOR both operands with the sign bit
    Term aFlipped = runtime.termNewOp2(HVM4Runtime::OP_XOR, a, signBit);
    Term bFlipped = runtime.termNewOp2(HVM4Runtime::OP_XOR, b, signBit);

    // Now use unsigned less-than
    return runtime.termNewOp2(HVM4Runtime::OP_LT, aFlipped, bFlipped);
}

Term makeNull(HVM4Runtime& runtime) {
    // Create a null value as an arity-0 constructor #Nul{}
    // This is different from ERA because ERA gets absorbed by operations.
    // Using C00 (constructor with arity 0) with our NIX_NULL tag.
    return runtime.termNewCtr(NIX_NULL, 0, nullptr);
}

bool isNull(Term term) {
    // Check for our null constructor encoding
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C00) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == NIX_NULL;
    }
    // Also check for ERA for backwards compatibility
    return tag == HVM4Runtime::TAG_ERA;
}

Term emitNullAwareEq(Term a, Term b, HVM4Runtime& runtime) {
    // For now, we'll use a simple approach:
    // If both terms are statically known to be null constructors, return 1.
    // If one is null and the other is NUM, return 0.
    // Otherwise, use the standard OP_EQ.
    //
    // Note: This is a compile-time check. For dynamic null checks at runtime,
    // we would need to emit more complex code using MAT/SWI.
    //
    // For the prototype, we rely on the fact that null is now a constructor,
    // and constructor equality in HVM4 compares tags. Two #Nul{} will match,
    // and #Nul{} vs NUM will not.

    // Use the standard equality - constructor tags will be compared
    return runtime.termNewOp2(HVM4Runtime::OP_EQ, a, b);
}

Term emitNullAwareNEq(Term a, Term b, HVM4Runtime& runtime) {
    return runtime.termNewOp2(HVM4Runtime::OP_NE, a, b);
}

// Helper: Build a curried lambda that ignores two arguments and returns result
static Term makeLam2Const(Term result, HVM4Runtime& runtime) {
    // λ_. λ_. result
    Term innerLam = runtime.termNewLam(result);
    return runtime.termNewLam(innerLam);
}

// Helper: Compare two BigInt field pairs (lo1, hi1) vs (lo2, hi2) for equality
// Returns (lo1 == lo2) && (hi1 == hi2)
static Term emitFieldsEqual(Term lo1, Term hi1, Term lo2, Term hi2, HVM4Runtime& runtime) {
    Term loEq = runtime.termNewOp2(HVM4Runtime::OP_EQ, lo1, lo2);
    Term hiEq = runtime.termNewOp2(HVM4Runtime::OP_EQ, hi1, hi2);
    return runtime.termNewAnd(loEq, hiEq);
}

// Helper: Compare two positive BigInt magnitudes for less-than
// For #Pos{lo1, hi1} < #Pos{lo2, hi2}: compare hi first, then lo
static Term emitPosLessThanPos(Term lo1, Term hi1, Term lo2, Term hi2, HVM4Runtime& runtime) {
    // (hi1 < hi2) || ((hi1 == hi2) && (lo1 < lo2))
    Term hiLt = runtime.termNewOp2(HVM4Runtime::OP_LT, hi1, hi2);
    Term hiEq = runtime.termNewOp2(HVM4Runtime::OP_EQ, hi1, hi2);
    Term loLt = runtime.termNewOp2(HVM4Runtime::OP_LT, lo1, lo2);
    Term hiEqAndLoLt = runtime.termNewAnd(hiEq, loLt);
    return runtime.termNewOr(hiLt, hiEqAndLoLt);
}

// Helper: Compare two negative BigInt magnitudes for less-than
// For #Neg{lo1, hi1} < #Neg{lo2, hi2}: larger magnitude = smaller value
// So we compare (hi1, lo1) > (hi2, lo2) to get <
static Term emitNegLessThanNeg(Term lo1, Term hi1, Term lo2, Term hi2, HVM4Runtime& runtime) {
    // (hi1 > hi2) || ((hi1 == hi2) && (lo1 > lo2))
    Term hiGt = runtime.termNewOp2(HVM4Runtime::OP_GT, hi1, hi2);
    Term hiEq = runtime.termNewOp2(HVM4Runtime::OP_EQ, hi1, hi2);
    Term loGt = runtime.termNewOp2(HVM4Runtime::OP_GT, lo1, lo2);
    Term hiEqAndLoGt = runtime.termNewAnd(hiEq, loGt);
    return runtime.termNewOr(hiGt, hiEqAndLoGt);
}

Term emitBigIntLessThan(Term a, Term b, HVM4Runtime& runtime) {
    // Full BigInt-aware less-than comparison using MAT pattern matching.
    //
    // Ordering: #Neg{...} < NUM(-2^31..-1) < NUM(0..2^31-1) < #Pos{...}
    //
    // Cases (9 total):
    // 1. #Neg{} < #Neg{}: larger magnitude = smaller value
    // 2. #Neg{} < NUM: always true
    // 3. #Neg{} < #Pos{}: always true
    // 4. NUM < #Neg{}: always false
    // 5. NUM < NUM: signed comparison
    // 6. NUM < #Pos{}: always true
    // 7. #Pos{} < #Neg{}: always false
    // 8. #Pos{} < NUM: always false
    // 9. #Pos{} < #Pos{}: compare hi, then lo

    Term one = HVM4Runtime::termNewNum(1);
    Term zero = HVM4Runtime::termNewNum(0);

    // === Case: a is #Neg{a_lo, a_hi} ===
    // Need to allocate lambda slots for a_lo and a_hi
    uint32_t aLoSlot = runtime.allocateLamSlot();
    uint32_t aHiSlot = runtime.allocateLamSlot();
    Term aLoVar = HVM4Runtime::termNewVar(aLoSlot);
    Term aHiVar = HVM4Runtime::termNewVar(aHiSlot);

    // Case: a is #Neg{}, b is #Neg{}
    uint32_t bLoSlot1 = runtime.allocateLamSlot();
    uint32_t bHiSlot1 = runtime.allocateLamSlot();
    Term bLoVar1 = HVM4Runtime::termNewVar(bLoSlot1);
    Term bHiVar1 = HVM4Runtime::termNewVar(bHiSlot1);
    Term negNegResult = emitNegLessThanNeg(aLoVar, aHiVar, bLoVar1, bHiVar1, runtime);
    Term negNegHiLam = runtime.finalizeLam(bHiSlot1, negNegResult);
    Term negNegLoLam = runtime.finalizeLam(bLoSlot1, negNegHiLam);

    // Case: a is #Neg{}, b is not #Neg{} -> always true
    Term negNotNegLam = runtime.termNewLam(one);

    // MAT for b when a is #Neg{}
    Term bMatWhenANeg = runtime.termNewMat(BIGINT_NEG, negNegLoLam, negNotNegLam);
    Term aIsNegResult = runtime.termNewApp(bMatWhenANeg, b);

    // Complete a_hi lambda for #Neg{} case
    Term aHiLamNeg = runtime.finalizeLam(aHiSlot, aIsNegResult);
    Term aLoLamNeg = runtime.finalizeLam(aLoSlot, aHiLamNeg);

    // === Case: a is not #Neg{} ===
    // Sub-case: a is #Pos{a_lo, a_hi}
    uint32_t aPosLoSlot = runtime.allocateLamSlot();
    uint32_t aPosHiSlot = runtime.allocateLamSlot();
    Term aPosLoVar = HVM4Runtime::termNewVar(aPosLoSlot);
    Term aPosHiVar = HVM4Runtime::termNewVar(aPosHiSlot);

    // Case: a is #Pos{}, b is #Pos{}
    uint32_t bPosLoSlot = runtime.allocateLamSlot();
    uint32_t bPosHiSlot = runtime.allocateLamSlot();
    Term bPosLoVar = HVM4Runtime::termNewVar(bPosLoSlot);
    Term bPosHiVar = HVM4Runtime::termNewVar(bPosHiSlot);
    Term posPosResult = emitPosLessThanPos(aPosLoVar, aPosHiVar, bPosLoVar, bPosHiVar, runtime);
    Term posPosHiLam = runtime.finalizeLam(bPosHiSlot, posPosResult);
    Term posPosLoLam = runtime.finalizeLam(bPosLoSlot, posPosHiLam);

    // Case: a is #Pos{}, b is not #Pos{} -> always false
    Term posNotPosLam = runtime.termNewLam(zero);

    // MAT for b when a is #Pos{}
    Term bMatWhenAPos = runtime.termNewMat(BIGINT_POS, posPosLoLam, posNotPosLam);
    Term aIsPosResult = runtime.termNewApp(bMatWhenAPos, b);

    Term aPosHiLam = runtime.finalizeLam(aPosHiSlot, aIsPosResult);
    Term aPosLoLam = runtime.finalizeLam(aPosLoSlot, aPosHiLam);

    // === Case: a is NUM (not #Neg{} or #Pos{}) ===
    uint32_t aNumSlot = runtime.allocateLamSlot();
    Term aNumVar = HVM4Runtime::termNewVar(aNumSlot);

    // Case: a is NUM, b is #Neg{} -> always false (NUM > #Neg{})
    Term numNegLam = makeLam2Const(zero, runtime);

    // Case: a is NUM, b is not #Neg{}
    // Sub-case: b is #Pos{} -> always true (NUM < #Pos{})
    Term numPosLam = makeLam2Const(one, runtime);

    // Sub-case: b is NUM -> signed comparison
    uint32_t bNumSlot = runtime.allocateLamSlot();
    Term bNumVar = HVM4Runtime::termNewVar(bNumSlot);
    Term numNumResult = emitSignedLessThan(aNumVar, bNumVar, runtime);
    Term numNumLam = runtime.finalizeLam(bNumSlot, numNumResult);

    // MAT for #Pos{} on b when a is NUM
    Term bPosMatWhenANum = runtime.termNewMat(BIGINT_POS, numPosLam, numNumLam);
    Term bNotNegWhenANumResult = runtime.termNewApp(bPosMatWhenANum, b);
    Term bNotNegWhenANumLam = runtime.termNewLam(bNotNegWhenANumResult);

    // MAT for #Neg{} on b when a is NUM
    Term bNegMatWhenANum = runtime.termNewMat(BIGINT_NEG, numNegLam, bNotNegWhenANumLam);
    Term aIsNumResult = runtime.termNewApp(bNegMatWhenANum, b);
    Term aNumLam = runtime.finalizeLam(aNumSlot, aIsNumResult);

    // MAT for #Pos{} on a (inner)
    Term aPosMatInner = runtime.termNewMat(BIGINT_POS, aPosLoLam, aNumLam);
    Term aNotNegResult = runtime.termNewApp(aPosMatInner, a);
    Term aNotNegLam = runtime.termNewLam(aNotNegResult);

    // MAT for #Neg{} on a (outer)
    Term aNegMat = runtime.termNewMat(BIGINT_NEG, aLoLamNeg, aNotNegLam);
    return runtime.termNewApp(aNegMat, a);
}

Term emitBigIntEquality(Term a, Term b, HVM4Runtime& runtime) {
    // Full BigInt-aware equality comparison using MAT pattern matching.
    //
    // Cases:
    // - NUM vs NUM: OP_EQ
    // - #Pos{} vs #Pos{}: compare both lo and hi fields
    // - #Neg{} vs #Neg{}: compare both lo and hi fields
    // - Mixed types: always false

    Term zero = HVM4Runtime::termNewNum(0);

    // === Case: a is #Pos{a_lo, a_hi} ===
    uint32_t aPosLoSlot = runtime.allocateLamSlot();
    uint32_t aPosHiSlot = runtime.allocateLamSlot();
    Term aPosLoVar = HVM4Runtime::termNewVar(aPosLoSlot);
    Term aPosHiVar = HVM4Runtime::termNewVar(aPosHiSlot);

    // Case: a is #Pos{}, b is #Pos{}
    uint32_t bPosLoSlot = runtime.allocateLamSlot();
    uint32_t bPosHiSlot = runtime.allocateLamSlot();
    Term bPosLoVar = HVM4Runtime::termNewVar(bPosLoSlot);
    Term bPosHiVar = HVM4Runtime::termNewVar(bPosHiSlot);
    Term posPosResult = emitFieldsEqual(aPosLoVar, aPosHiVar, bPosLoVar, bPosHiVar, runtime);
    Term posPosHiLam = runtime.finalizeLam(bPosHiSlot, posPosResult);
    Term posPosLoLam = runtime.finalizeLam(bPosLoSlot, posPosHiLam);

    // Case: a is #Pos{}, b is not #Pos{} -> false
    Term posNotPosLam = runtime.termNewLam(zero);

    Term bMatWhenAPos = runtime.termNewMat(BIGINT_POS, posPosLoLam, posNotPosLam);
    Term aIsPosResult = runtime.termNewApp(bMatWhenAPos, b);
    Term aPosHiLam = runtime.finalizeLam(aPosHiSlot, aIsPosResult);
    Term aPosLoLam = runtime.finalizeLam(aPosLoSlot, aPosHiLam);

    // === Case: a is #Neg{a_lo, a_hi} ===
    uint32_t aNegLoSlot = runtime.allocateLamSlot();
    uint32_t aNegHiSlot = runtime.allocateLamSlot();
    Term aNegLoVar = HVM4Runtime::termNewVar(aNegLoSlot);
    Term aNegHiVar = HVM4Runtime::termNewVar(aNegHiSlot);

    // Case: a is #Neg{}, b is #Neg{}
    uint32_t bNegLoSlot = runtime.allocateLamSlot();
    uint32_t bNegHiSlot = runtime.allocateLamSlot();
    Term bNegLoVar = HVM4Runtime::termNewVar(bNegLoSlot);
    Term bNegHiVar = HVM4Runtime::termNewVar(bNegHiSlot);
    Term negNegResult = emitFieldsEqual(aNegLoVar, aNegHiVar, bNegLoVar, bNegHiVar, runtime);
    Term negNegHiLam = runtime.finalizeLam(bNegHiSlot, negNegResult);
    Term negNegLoLam = runtime.finalizeLam(bNegLoSlot, negNegHiLam);

    // Case: a is #Neg{}, b is not #Neg{} -> false
    Term negNotNegLam = runtime.termNewLam(zero);

    Term bMatWhenANeg = runtime.termNewMat(BIGINT_NEG, negNegLoLam, negNotNegLam);
    Term aIsNegResult = runtime.termNewApp(bMatWhenANeg, b);
    Term aNegHiLam = runtime.finalizeLam(aNegHiSlot, aIsNegResult);
    Term aNegLoLam = runtime.finalizeLam(aNegLoSlot, aNegHiLam);

    // === Case: a is NUM ===
    uint32_t aNumSlot = runtime.allocateLamSlot();
    Term aNumVar = HVM4Runtime::termNewVar(aNumSlot);

    // Case: a is NUM, b is #Pos{} -> false
    Term numPosLam = makeLam2Const(zero, runtime);

    // Case: a is NUM, b is #Neg{} -> false
    Term numNegLam = makeLam2Const(zero, runtime);

    // Case: a is NUM, b is NUM -> OP_EQ
    uint32_t bNumSlot = runtime.allocateLamSlot();
    Term bNumVar = HVM4Runtime::termNewVar(bNumSlot);
    Term numNumResult = runtime.termNewOp2(HVM4Runtime::OP_EQ, aNumVar, bNumVar);
    Term numNumLam = runtime.finalizeLam(bNumSlot, numNumResult);

    // MAT for #Pos{} on b when a is NUM
    Term bPosMatWhenANum = runtime.termNewMat(BIGINT_POS, numPosLam, numNumLam);
    Term bNotNegWhenANumResult = runtime.termNewApp(bPosMatWhenANum, b);
    Term bNotNegWhenANumLam = runtime.termNewLam(bNotNegWhenANumResult);

    // MAT for #Neg{} on b when a is NUM
    Term bNegMatWhenANum = runtime.termNewMat(BIGINT_NEG, numNegLam, bNotNegWhenANumLam);
    Term aIsNumResult = runtime.termNewApp(bNegMatWhenANum, b);
    Term aNumLam = runtime.finalizeLam(aNumSlot, aIsNumResult);

    // === Outer structure ===
    // MAT for #Neg{} on a
    Term aNegMatInner = runtime.termNewMat(BIGINT_NEG, aNegLoLam, aNumLam);
    Term aNotPosResult = runtime.termNewApp(aNegMatInner, a);
    Term aNotPosLam = runtime.termNewLam(aNotPosResult);

    // MAT for #Pos{} on a
    Term aPosMat = runtime.termNewMat(BIGINT_POS, aPosLoLam, aNotPosLam);
    return runtime.termNewApp(aPosMat, a);
}

Term emitBigIntInequality(Term a, Term b, HVM4Runtime& runtime) {
    // BigInt-aware inequality - just negate equality
    Term eq = emitBigIntEquality(a, b, runtime);
    // NOT(x) = if x then 0 else 1
    // Build: (eq == 0) which gives us !eq
    Term zero = HVM4Runtime::termNewNum(0);
    return runtime.termNewOp2(HVM4Runtime::OP_EQ, eq, zero);
}

Term emitBigIntLessThanSimple(Term a, Term b, HVM4Runtime& runtime) {
    // Simplified version: only handles NUM vs NUM case correctly.
    // BigInt constructors will be compared incorrectly (just returns 0).
    // This is for debugging the MAT structure.

    Term zero = HVM4Runtime::termNewNum(0);

    // Allocate slot for a when it's NUM
    uint32_t aNumSlot = runtime.allocateLamSlot();
    Term aNumVar = HVM4Runtime::termNewVar(aNumSlot);

    // Allocate slot for b when it's NUM
    uint32_t bNumSlot = runtime.allocateLamSlot();
    Term bNumVar = HVM4Runtime::termNewVar(bNumSlot);

    // NUM vs NUM: signed comparison
    Term numNumResult = emitSignedLessThan(aNumVar, bNumVar, runtime);
    Term bNumLam = runtime.finalizeLam(bNumSlot, numNumResult);

    // If b is #Pos{}, return 1 (NUM < #Pos{})
    Term one = HVM4Runtime::termNewNum(1);
    Term bPosLam = makeLam2Const(one, runtime);

    // MAT on b for #Pos{}: if matches -> 1, else -> signed comparison
    Term bPosMat = runtime.termNewMat(BIGINT_POS, bPosLam, bNumLam);
    Term aNumBody = runtime.termNewApp(bPosMat, b);
    Term aNumLam = runtime.finalizeLam(aNumSlot, aNumBody);

    // If a is #Pos{}, return 0 (#Pos{} > everything else for simplicity)
    Term aPosLam = makeLam2Const(zero, runtime);

    // MAT on a for #Pos{}: if matches -> 0, else -> check b
    Term aPosMat = runtime.termNewMat(BIGINT_POS, aPosLam, aNumLam);
    return runtime.termNewApp(aPosMat, a);
}

// ============================================================================
// Float Encoding
// ============================================================================

Term encodeFloat(double value, HVM4Runtime& runtime) {
    // Reinterpret the double as a 64-bit unsigned integer (IEEE 754 bit pattern)
    uint64_t bits;
    static_assert(sizeof(double) == sizeof(uint64_t), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));

    // Split into low and high 32-bit words
    uint32_t lo = static_cast<uint32_t>(bits & 0xFFFFFFFF);
    uint32_t hi = static_cast<uint32_t>(bits >> 32);

    // Create constructor: #Flt{lo, hi}
    Term args[2] = {HVM4Runtime::termNewNum(lo), HVM4Runtime::termNewNum(hi)};
    return runtime.termNewCtr(NIX_FLT, 2, args);
}

std::optional<double> decodeFloat(Term term, const HVM4Runtime& runtime) {
    // Check if it's an arity-2 constructor with tag NIX_FLT
    if (HVM4Runtime::termTag(term) != HVM4Runtime::TAG_C02) {
        return std::nullopt;
    }

    uint32_t ext = HVM4Runtime::termExt(term);
    if (ext != NIX_FLT) {
        return std::nullopt;
    }

    // Get the lo and hi values from heap
    uint32_t loc = HVM4Runtime::termVal(term);
    Term loTerm = runtime.load(loc);
    Term hiTerm = runtime.load(loc + 1);

    // Both should be NUM terms
    if (HVM4Runtime::termTag(loTerm) != HVM4Runtime::TAG_NUM ||
        HVM4Runtime::termTag(hiTerm) != HVM4Runtime::TAG_NUM) {
        return std::nullopt;
    }

    uint32_t lo = HVM4Runtime::termVal(loTerm);
    uint32_t hi = HVM4Runtime::termVal(hiTerm);

    // Reconstruct the 64-bit value
    uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;

    // Reinterpret as double
    double result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool isFloat(Term term) {
    return HVM4Runtime::termTag(term) == HVM4Runtime::TAG_C02 &&
           HVM4Runtime::termExt(term) == NIX_FLT;
}

}  // namespace nix::hvm4
