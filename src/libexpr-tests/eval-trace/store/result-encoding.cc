/**
 * Tests for float encoding precision in CachedResult codec.
 *
 * These tests exercise the float_t encode/decode path in
 * SqliteTraceStorage::encodeCachedResult / SqliteTraceStorage::decodeCachedResult.
 *
 * BUG-10: std::to_string uses %f format (6 significant decimal digits),
 * truncating values like 3.141592653589793 to 3.141593. std::to_chars with
 * no format argument produces the shortest round-trip representation, so
 * decode(encode(x)) == x bit-for-bit.
 *
 * Tests 1, 2, 5 are BUG-10 regression guards (would fail under std::to_string).
 * Tests 3, 4 are precision boundary tests (pass with either implementation
 * because -0.0 and infinity have no fractional digits to lose).
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Float encoding round-trip helpers ───────────────────────────────

namespace {

/**
 * Encode a float_t CachedResult, then decode it and return the recovered
 * double.  Uses the same SqliteTraceStorage encode/decode pair that the production
 * cache path uses, accessed through the test-only TraceStorageTestAccess friend.
 */
double encodeDecodeFloat(SqliteTraceStorage & store, double value)
{
    CachedResult input = float_t{value};
    auto encoded = TraceStorageTestAccess::encodeCachedResult(store, input);
    // Build the minimal ResultPayload that decodeCachedResult expects.
    SqliteTraceStorage::ResultPayload payload{
        .type = encoded.type,
        .encodingVersion = encoded.encodingVersion,
        .payload = encoded.payload,
        .auxContext = encoded.auxContext,
    };
    auto decoded = TraceStorageTestAccess::decodeCachedResult(store, payload);
    EXPECT_TRUE(std::holds_alternative<float_t>(decoded));
    return std::get<float_t>(decoded).x;
}

} // namespace

class FloatEncodingTest : public TraceStoreFixture {};

// ── Test 1: Full-precision round-trip ────────────────────────────────

TEST_F(FloatEncodingTest, FullPrecision_Pi_RoundTrips)
{
    // Pi to 16 significant digits — exceeds std::to_string's 6-digit limit.
    // BUG-10: std::to_string("3.141592653589793") → "3.141593", so decode
    // would return 3.141593, not 3.141592653589793.
    const double value = 3.141592653589793;
    auto db = makeDb();
    double result = encodeDecodeFloat(*db, value);

    // Bit-for-bit equality via memcmp rules out "close but not equal" mismatches
    // from intermediate rounding that EXPECT_EQ with doubles might mask.
    uint64_t orig_bits, result_bits;
    std::memcpy(&orig_bits, &value, sizeof(orig_bits));
    std::memcpy(&result_bits, &result, sizeof(result_bits));
    EXPECT_EQ(orig_bits, result_bits)
        << "Float round-trip lost precision: encoded "
        << value << " but decoded " << result;
}

// ── Test 2: Very small value preserved ──────────────────────────────

TEST_F(FloatEncodingTest, SmallValue_1eNeg300_Preserved)
{
    // 1e-300 is well below the range std::to_string can faithfully represent
    // with 6 significant digits (%f format saturates to "0.000000").
    const double value = 1e-300;
    auto db = makeDb();
    double result = encodeDecodeFloat(*db, value);

    uint64_t orig_bits, result_bits;
    std::memcpy(&orig_bits, &value, sizeof(orig_bits));
    std::memcpy(&result_bits, &result, sizeof(result_bits));
    EXPECT_EQ(orig_bits, result_bits)
        << "Small float round-trip lost precision: " << value << " vs " << result;
}

// ── Test 3: Negative zero preserved ─────────────────────────────────

TEST_F(FloatEncodingTest, NegativeZero_SignBit_Preserved)
{
    // -0.0 and +0.0 compare equal as doubles but have different bit patterns.
    // std::to_string(-0.0) = "-0.000000"; std::to_chars(-0.0) = "-0".
    // Both decode to -0.0, but we verify the round-trip is exact at the
    // bit level so the sign is not silently discarded.
    const double value = -0.0;
    auto db = makeDb();
    double result = encodeDecodeFloat(*db, value);

    uint64_t orig_bits, result_bits;
    std::memcpy(&orig_bits, &value, sizeof(orig_bits));
    std::memcpy(&result_bits, &result, sizeof(result_bits));
    EXPECT_EQ(orig_bits, result_bits)
        << "Negative zero lost its sign bit after round-trip";
}

// ── Test 4: Infinity preserved ───────────────────────────────────────

TEST_F(FloatEncodingTest, Infinity_PosNeg_Preserved)
{
    // std::stod (used on the decode side) handles "inf" and "-inf" correctly.
    // std::to_chars emits "inf" / "-inf" which std::stod understands.
    auto db = makeDb();

    {
        const double pos_inf = std::numeric_limits<double>::infinity();
        double result = encodeDecodeFloat(*db, pos_inf);
        EXPECT_TRUE(std::isinf(result) && result > 0)
            << "+Inf round-trip failed: got " << result;
    }
    {
        const double neg_inf = -std::numeric_limits<double>::infinity();
        double result = encodeDecodeFloat(*db, neg_inf);
        EXPECT_TRUE(std::isinf(result) && result < 0)
            << "-Inf round-trip failed: got " << result;
    }
}

// ── Test 6: Quiet NaN round-trip (G-10) ─────────────────────────────
//
// std::to_chars produces "nan" for quiet NaN; std::stod("nan") recovers a
// NaN bit-pattern. Guards against codec changes that silently drop NaN to 0.0.

TEST_F(FloatEncodingTest, NaN_Quiet_RoundTrips)
{
    auto db = makeDb();
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    double result = encodeDecodeFloat(*db, nan_val);
    EXPECT_TRUE(std::isnan(result))
        << "NaN did not survive encode/decode; got " << result;
}

// ── Test 7: Negative NaN — codec only guarantees isnan(result) ───────
//
// Sign-bit preservation for NaN is not required: std::to_chars may
// canonicalize the bit pattern. The codec only guarantees isnan(result),
// not that the sign bit is preserved.

TEST_F(FloatEncodingTest, NaN_Negative_StillIsNaN)
{
    auto db = makeDb();
    uint64_t bits = 0xFFF8000000000000ULL; // IEEE 754 canonical -qNaN
    double neg_nan;
    std::memcpy(&neg_nan, &bits, sizeof(neg_nan));
    double result = encodeDecodeFloat(*db, neg_nan);
    EXPECT_TRUE(std::isnan(result));
}

// ── Test 5: Subnormal (denormalized) value — decoder limitation ──────

TEST_F(FloatEncodingTest, Subnormal_Decoder_ThrowsOutOfRange)
{
    // denorm_min (~5e-324) is correctly encoded by std::to_chars, but the
    // decoder (std::stod) throws "out of range" for values this small on
    // some platforms. This is a pre-existing decoder limitation, not a
    // regression from the BUG-10 fix.
    //
    // This test documents the limitation: subnormal doubles cannot
    // round-trip through the current encode/decode path.
    const double value = std::numeric_limits<double>::denorm_min();
    auto db = makeDb();

    // The encode succeeds; the decode throws std::out_of_range.
    CachedResult input = float_t{value};
    auto encoded = TraceStorageTestAccess::encodeCachedResult(*db, input);

    // Verify the encoder produced a non-empty payload (std::to_chars worked).
    EXPECT_FALSE(encoded.payload.empty())
        << "Encoder should produce a valid string for denorm_min";

    // The decoder throws — document this as a known limitation.
    SqliteTraceStorage::ResultPayload payload{
        .type = encoded.type,
        .encodingVersion = encoded.encodingVersion,
        .payload = encoded.payload,
        .auxContext = encoded.auxContext,
    };
    EXPECT_THROW(
        TraceStorageTestAccess::decodeCachedResult(*db, payload),
        std::out_of_range)
        << "Known limitation: std::stod cannot parse subnormal doubles";
}

} // namespace nix::eval_trace
