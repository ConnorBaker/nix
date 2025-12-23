/**
 * HVM4 BigInt Tests
 *
 * Tests for 64-bit integer encoding/decoding in HVM4's 32-bit term format.
 * HVM4 uses constructor terms (C02) to represent integers that don't fit
 * in the 32-bit NUM format.
 *
 * Encoding scheme:
 * - Small integers (fits in int32_t): Native NUM term
 * - Large positive integers: C02 with BIGINT_POS, low 32 bits, high 32 bits
 * - Large negative integers: C02 with BIGINT_NEG, low 32 bits, high 32 bits
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Size Classification Tests
// =============================================================================

TEST_F(HVM4BigIntTest, SmallIntFits) {
    EXPECT_TRUE(fitsInSmallInt(0));
    EXPECT_TRUE(fitsInSmallInt(1));
    EXPECT_TRUE(fitsInSmallInt(-1));
    EXPECT_TRUE(fitsInSmallInt(INT32_MAX));
    EXPECT_TRUE(fitsInSmallInt(INT32_MIN));
}

TEST_F(HVM4BigIntTest, LargeIntDoesNotFit) {
    EXPECT_FALSE(fitsInSmallInt(static_cast<int64_t>(INT32_MAX) + 1));
    EXPECT_FALSE(fitsInSmallInt(static_cast<int64_t>(INT32_MIN) - 1));
    EXPECT_FALSE(fitsInSmallInt(INT64_MAX));
    EXPECT_FALSE(fitsInSmallInt(INT64_MIN));
}

// =============================================================================
// Encoding Tests
// =============================================================================

TEST_F(HVM4BigIntTest, EncodeSmallPositive) {
    Term t = encodeInt64(42, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(static_cast<int32_t>(HVM4Runtime::termVal(t)), 42);
}

TEST_F(HVM4BigIntTest, EncodeSmallNegative) {
    Term t = encodeInt64(-42, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(static_cast<int32_t>(HVM4Runtime::termVal(t)), -42);
}

TEST_F(HVM4BigIntTest, EncodeZero) {
    Term t = encodeInt64(0, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(t), 0u);
}

TEST_F(HVM4BigIntTest, EncodeLargePositive) {
    int64_t val = static_cast<int64_t>(INT32_MAX) + 1000;
    Term t = encodeInt64(val, runtime);
    // Should be a constructor, not a NUM
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_C02);
    EXPECT_EQ(HVM4Runtime::termExt(t), BIGINT_POS);
}

TEST_F(HVM4BigIntTest, EncodeLargeNegative) {
    int64_t val = static_cast<int64_t>(INT32_MIN) - 1000;
    Term t = encodeInt64(val, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_C02);
    EXPECT_EQ(HVM4Runtime::termExt(t), BIGINT_NEG);
}

// =============================================================================
// Roundtrip Tests
// =============================================================================

TEST_F(HVM4BigIntTest, RoundtripSmall) {
    for (int64_t val : {0LL, 1LL, -1LL, 42LL, -42LL, 1000000LL, -1000000LL}) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed to decode " << val;
        EXPECT_EQ(*decoded, val) << "Roundtrip failed for " << val;
    }
}

TEST_F(HVM4BigIntTest, RoundtripInt32Bounds) {
    Term tMax = encodeInt64(INT32_MAX, runtime);
    auto decodedMax = decodeInt64(tMax, runtime);
    ASSERT_TRUE(decodedMax.has_value());
    EXPECT_EQ(*decodedMax, INT32_MAX);

    Term tMin = encodeInt64(INT32_MIN, runtime);
    auto decodedMin = decodeInt64(tMin, runtime);
    ASSERT_TRUE(decodedMin.has_value());
    EXPECT_EQ(*decodedMin, INT32_MIN);
}

TEST_F(HVM4BigIntTest, RoundtripLarge) {
    for (int64_t val : {
        static_cast<int64_t>(INT32_MAX) + 1,
        static_cast<int64_t>(INT32_MAX) + 1000000,
        static_cast<int64_t>(INT32_MIN) - 1,
        static_cast<int64_t>(INT32_MIN) - 1000000,
        INT64_MAX,
        INT64_MIN,
        INT64_MAX / 2,
        INT64_MIN / 2
    }) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed to decode " << val;
        EXPECT_EQ(*decoded, val) << "Roundtrip failed for " << val;
        runtime.reset();
    }
}

// =============================================================================
// Boundary Tests (Session 14)
// =============================================================================

// Value just above INT32_MAX boundary
TEST_F(HVM4BigIntTest, BoundaryJustAboveInt32Max) {
    int64_t val = static_cast<int64_t>(INT32_MAX) + 1;
    Term t = encodeInt64(val, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_C02);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Value just below INT32_MIN boundary
TEST_F(HVM4BigIntTest, BoundaryJustBelowInt32Min) {
    int64_t val = static_cast<int64_t>(INT32_MIN) - 1;
    Term t = encodeInt64(val, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_C02);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Power of 2 boundaries
TEST_F(HVM4BigIntTest, PowerOfTwoBoundaries) {
    for (int64_t val : {1LL << 31, 1LL << 32, 1LL << 40, 1LL << 50, 1LL << 62}) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for 2^" << val;
        EXPECT_EQ(*decoded, val);
        runtime.reset();
    }
}

// Negative power of 2 boundaries
TEST_F(HVM4BigIntTest, NegativePowerOfTwoBoundaries) {
    for (int64_t val : {-(1LL << 31), -(1LL << 32), -(1LL << 40), -(1LL << 50)}) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for " << val;
        EXPECT_EQ(*decoded, val);
        runtime.reset();
    }
}

// =============================================================================
// Session 25: Extended BigInt Tests
// =============================================================================

// Specific large value tests
TEST_F(HVM4BigIntTest, Session25LargePositiveSpecific) {
    int64_t val = 9999999999999LL;
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

TEST_F(HVM4BigIntTest, Session25LargeNegativeSpecific) {
    int64_t val = -9999999999999LL;
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Trillion-scale values
TEST_F(HVM4BigIntTest, Session25Trillion) {
    int64_t val = 1000000000000LL;  // 1 trillion
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

TEST_F(HVM4BigIntTest, Session25NegativeTrillion) {
    int64_t val = -1000000000000LL;
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Quadrillion-scale values
TEST_F(HVM4BigIntTest, Session25Quadrillion) {
    int64_t val = 1000000000000000LL;  // 1 quadrillion
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Sequential values around boundaries
TEST_F(HVM4BigIntTest, Session25SequentialAroundInt32Max) {
    for (int64_t val = INT32_MAX - 2; val <= static_cast<int64_t>(INT32_MAX) + 2; ++val) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for " << val;
        EXPECT_EQ(*decoded, val) << "Mismatch for " << val;
        runtime.reset();
    }
}

TEST_F(HVM4BigIntTest, Session25SequentialAroundInt32Min) {
    for (int64_t val = static_cast<int64_t>(INT32_MIN) - 2; val <= INT32_MIN + 2; ++val) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for " << val;
        EXPECT_EQ(*decoded, val) << "Mismatch for " << val;
        runtime.reset();
    }
}

// Powers of 10
TEST_F(HVM4BigIntTest, Session25PowersOfTen) {
    for (int exp = 0; exp <= 18; ++exp) {
        int64_t val = 1;
        for (int i = 0; i < exp; ++i) val *= 10;
        if (val > 0) {  // Check for overflow
            Term t = encodeInt64(val, runtime);
            auto decoded = decodeInt64(t, runtime);
            ASSERT_TRUE(decoded.has_value()) << "Failed for 10^" << exp;
            EXPECT_EQ(*decoded, val) << "Mismatch for 10^" << exp;
            runtime.reset();
        }
    }
}

// Negative powers of 10
TEST_F(HVM4BigIntTest, Session25NegativePowersOfTen) {
    for (int exp = 0; exp <= 18; ++exp) {
        int64_t val = -1;
        for (int i = 0; i < exp; ++i) val *= 10;
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for -10^" << exp;
        EXPECT_EQ(*decoded, val) << "Mismatch for -10^" << exp;
        runtime.reset();
    }
}

// Alternating bit patterns
TEST_F(HVM4BigIntTest, Session25AlternatingBits) {
    int64_t val1 = 0x5555555555555555LL;  // 0101...
    Term t1 = encodeInt64(val1, runtime);
    auto decoded1 = decodeInt64(t1, runtime);
    ASSERT_TRUE(decoded1.has_value());
    EXPECT_EQ(*decoded1, val1);

    runtime.reset();

    int64_t val2 = 0x2AAAAAAAAAAAAAALL;  // 1010... (limited to positive)
    Term t2 = encodeInt64(val2, runtime);
    auto decoded2 = decodeInt64(t2, runtime);
    ASSERT_TRUE(decoded2.has_value());
    EXPECT_EQ(*decoded2, val2);
}

// All ones patterns
TEST_F(HVM4BigIntTest, Session25AllOnes32) {
    int64_t val = 0xFFFFFFFFLL;  // 32 ones
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

TEST_F(HVM4BigIntTest, Session25AllOnes48) {
    int64_t val = 0xFFFFFFFFFFFFLL;  // 48 ones
    Term t = encodeInt64(val, runtime);
    auto decoded = decodeInt64(t, runtime);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, val);
}

// Specific problematic values
TEST_F(HVM4BigIntTest, Session25ProblematicValues) {
    // Values that might cause issues with sign extension
    for (int64_t val : {
        0x7FFFFFFFLL,      // INT32_MAX
        0x80000000LL,      // INT32_MAX + 1
        0x100000000LL,     // 2^32
        0x1FFFFFFFFLL,     // 2^33 - 1
        -0x80000000LL,     // INT32_MIN
        -0x80000001LL,     // INT32_MIN - 1
        -0x100000000LL,    // -2^32
    }) {
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for " << val;
        EXPECT_EQ(*decoded, val) << "Mismatch for " << val;
        runtime.reset();
    }
}

// Near INT64_MAX/MIN
TEST_F(HVM4BigIntTest, Session25NearInt64Limits) {
    for (int64_t offset = 0; offset <= 10; ++offset) {
        int64_t val = INT64_MAX - offset;
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for INT64_MAX-" << offset;
        EXPECT_EQ(*decoded, val);
        runtime.reset();
    }

    for (int64_t offset = 0; offset <= 10; ++offset) {
        int64_t val = INT64_MIN + offset;
        Term t = encodeInt64(val, runtime);
        auto decoded = decodeInt64(t, runtime);
        ASSERT_TRUE(decoded.has_value()) << "Failed for INT64_MIN+" << offset;
        EXPECT_EQ(*decoded, val);
        runtime.reset();
    }
}

// Small integer boundary verification
TEST_F(HVM4BigIntTest, Session25SmallIntBoundaries) {
    // Verify small ints use NUM tag, large use C02
    Term tSmall = encodeInt64(INT32_MAX, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(tSmall), HVM4Runtime::TAG_NUM);

    runtime.reset();

    Term tLarge = encodeInt64(static_cast<int64_t>(INT32_MAX) + 1, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(tLarge), HVM4Runtime::TAG_C02);

    runtime.reset();

    Term tSmallNeg = encodeInt64(INT32_MIN, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(tSmallNeg), HVM4Runtime::TAG_NUM);

    runtime.reset();

    Term tLargeNeg = encodeInt64(static_cast<int64_t>(INT32_MIN) - 1, runtime);
    EXPECT_EQ(HVM4Runtime::termTag(tLargeNeg), HVM4Runtime::TAG_C02);
}

// Sign preservation
TEST_F(HVM4BigIntTest, Session25SignPreservation) {
    // Verify positive values stay positive
    int64_t posVal = 5000000000LL;
    Term tPos = encodeInt64(posVal, runtime);
    auto decodedPos = decodeInt64(tPos, runtime);
    ASSERT_TRUE(decodedPos.has_value());
    EXPECT_GT(*decodedPos, 0);

    runtime.reset();

    // Verify negative values stay negative
    int64_t negVal = -5000000000LL;
    Term tNeg = encodeInt64(negVal, runtime);
    auto decodedNeg = decodeInt64(tNeg, runtime);
    ASSERT_TRUE(decodedNeg.has_value());
    EXPECT_LT(*decodedNeg, 0);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
