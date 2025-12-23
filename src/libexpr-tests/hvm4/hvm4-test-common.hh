/**
 * HVM4 Test Common Header
 *
 * Shared includes and declarations for all HVM4 test files.
 * Include this header in each HVM4 test file.
 */

#pragma once

#include "nix/expr/tests/libexpr.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/expr/config.hh"

#if NIX_USE_HVM4

#include "nix/expr/hvm4/hvm4-backend.hh"
#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"
#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-list.hh"
#include "nix/expr/hvm4/hvm4-result.hh"

namespace nix {
namespace hvm4 {

/**
 * Test fixture for low-level HVM4 runtime tests.
 * Tests term construction and evaluation without Nix parsing.
 */
class HVM4RuntimeTest : public ::testing::Test {
protected:
    HVM4Runtime runtime{1 << 20}; // 1M terms
};

/**
 * Test fixture for BigInt encoding/decoding tests.
 * Tests 64-bit integer representation in HVM4's 32-bit term format.
 */
class HVM4BigIntTest : public ::testing::Test {
protected:
    HVM4Runtime runtime{1 << 20};
};

/**
 * Test fixture for full HVM4 backend tests.
 * Tests complete Nix expression parsing, compilation, and evaluation.
 */
class HVM4BackendTest : public LibExprTest {
protected:
    HVM4Backend backend{state};
};

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
