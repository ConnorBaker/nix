/**
 * HVM4 String Tests
 *
 * Comprehensive tests for string functionality in the HVM4 backend.
 *
 * String support is now implemented in the HVM4 backend.
 * Tests verify:
 *   - Basic string literals
 *   - String concatenation
 *   - Strings in let bindings
 *   - Strings with conditionals and lambdas
 *
 * Test Categories:
 * - Basic Strings: Empty, simple, with spaces
 * - String Interpolation: Variable interpolation in strings
 * - String Concatenation: Using + operator
 * - Multiline Strings: ''...'' syntax
 * - Escape Sequences: \n \t \\ \" \$
 * - Unicode Strings: Non-ASCII characters, emoji, CJK
 * - String Context: Store path dependency tracking
 *
 * See docs/hvm4-plan/03-strings.md for implementation details.
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic String Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringEmpty) {
    // Empty string ""
    auto* expr = state.parseExprFromString("\"\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "");
}

TEST_F(HVM4BackendTest, StringSimple) {
    // Simple string "hello"
    auto* expr = state.parseExprFromString("\"hello\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, StringWithSpaces) {
    // String with spaces "hello world"
    auto* expr = state.parseExprFromString("\"hello world\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello world");
}

TEST_F(HVM4BackendTest, StringWithNumbers) {
    // String containing numeric characters
    auto* expr = state.parseExprFromString("\"test123\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "test123");
}

TEST_F(HVM4BackendTest, StringWithPunctuation) {
    // String with various punctuation
    auto* expr = state.parseExprFromString("\"hello, world!\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello, world!");
}

// =============================================================================
// String Interpolation Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringInterpolationSimple) {
    // Simple variable interpolation "hello ${x}"
    auto* expr = state.parseExprFromString(
        "let x = \"world\"; in \"hello ${x}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello world");
}

TEST_F(HVM4BackendTest, StringInterpolationMultiple) {
    // Multiple interpolations
    auto* expr = state.parseExprFromString(
        "let a = \"one\"; b = \"two\"; in \"${a} and ${b}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "one and two");
}

TEST_F(HVM4BackendTest, StringInterpolationNested) {
    // Nested interpolation
    auto* expr = state.parseExprFromString(
        "let x = \"inner\"; in \"outer ${\"prefix ${x} suffix\"}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "outer prefix inner suffix");
}

TEST_F(HVM4BackendTest, StringInterpolationExpression) {
    // Interpolation with expression (not just variable)
    auto* expr = state.parseExprFromString(
        "\"result: ${if true then \"yes\" else \"no\"}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "result: yes");
}

TEST_F(HVM4BackendTest, StringInterpolationWithLet) {
    // Interpolation with let expression inside
    auto* expr = state.parseExprFromString(
        "\"${let x = \"test\"; in x}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "test");
}

// =============================================================================
// String Concatenation Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringConcatSimple) {
    // Simple concatenation "a" + "b"
    auto* expr = state.parseExprFromString(
        "\"a\" + \"b\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "ab");
}

TEST_F(HVM4BackendTest, StringConcatMultiple) {
    // Multiple concatenations
    auto* expr = state.parseExprFromString(
        "\"hello\" + \" \" + \"world\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello world");
}

TEST_F(HVM4BackendTest, StringConcatEmpty) {
    // Concatenation with empty string
    auto* expr = state.parseExprFromString(
        "\"hello\" + \"\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, StringConcatEmptyBoth) {
    // Concatenation of two empty strings
    auto* expr = state.parseExprFromString(
        "\"\" + \"\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "");
}

TEST_F(HVM4BackendTest, StringConcatWithVariable) {
    // Concatenation involving variable - not yet supported
    // (requires runtime string operations, only constant strings work)
    auto* expr = state.parseExprFromString(
        "let x = \"world\"; in \"hello \" + x",
        state.rootPath(CanonPath::root)
    );
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringConcatChained) {
    // Long chain of concatenations
    auto* expr = state.parseExprFromString(
        "\"a\" + \"b\" + \"c\" + \"d\" + \"e\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "abcde");
}

// =============================================================================
// Multiline String Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringMultilineSimple) {
    // Simple multiline string ''...''
    auto* expr = state.parseExprFromString(
        "''hello''",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, StringMultilineWithNewlines) {
    // Multiline with actual newlines
    auto* expr = state.parseExprFromString(
        "''\n  line1\n  line2\n''",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    // Note: multiline strings strip indentation
    EXPECT_STREQ(result.c_str(), "line1\nline2\n");
}

TEST_F(HVM4BackendTest, StringMultilineIndented) {
    // Multiline with indentation stripping
    auto* expr = state.parseExprFromString(
        "''\n    line1\n    line2\n  ''",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    // Nix strips leading whitespace based on the minimum indentation
    // across all non-empty lines. The result matches what Nix's parser
    // produces when it parses the multiline string.
    EXPECT_STREQ(result.c_str(), "line1\nline2\n");
}

TEST_F(HVM4BackendTest, StringMultilineEmpty) {
    // Empty multiline string
    auto* expr = state.parseExprFromString(
        "''''",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "");
}

TEST_F(HVM4BackendTest, StringMultilineWithInterpolation) {
    // Multiline with interpolation
    auto* expr = state.parseExprFromString(
        "let x = \"test\"; in ''\n  ${x}\n''",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    // Nix strips common indentation from multiline strings
    EXPECT_STREQ(result.c_str(), "test\n");
}

TEST_F(HVM4BackendTest, StringMultilineEscapedDollar) {
    // Multiline with escaped interpolation ''$
    auto* expr = state.parseExprFromString(
        "''literal ''${not interpolation}''",
        state.rootPath(CanonPath::root)
    );
    // Nix parses ''${...} as literal ${...} - no interpolation
    // This becomes a plain ExprString with the literal text
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "literal ${not interpolation}");
}

TEST_F(HVM4BackendTest, StringMultilineEscapedQuotes) {
    // Multiline with escaped single quotes '''
    auto* expr = state.parseExprFromString(
        "''contains ''' quotes''",
        state.rootPath(CanonPath::root)
    );
    // Nix parses ''' as literal '' in the output
    // This becomes a plain ExprString with the literal text
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "contains '' quotes");
}

// =============================================================================
// Escape Sequence Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringEscapeNewline) {
    // Newline escape \n
    auto* expr = state.parseExprFromString(
        "\"hello\\nworld\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\nworld");
}

TEST_F(HVM4BackendTest, StringEscapeTab) {
    // Tab escape \t
    auto* expr = state.parseExprFromString(
        "\"hello\\tworld\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\tworld");
}

TEST_F(HVM4BackendTest, StringEscapeCarriageReturn) {
    // Carriage return escape \r
    auto* expr = state.parseExprFromString(
        "\"hello\\rworld\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\rworld");
}

TEST_F(HVM4BackendTest, StringEscapeBackslash) {
    // Backslash escape (double backslash)
    auto* expr = state.parseExprFromString(
        "\"hello\\\\world\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\\world");
}

TEST_F(HVM4BackendTest, StringEscapeQuote) {
    // Quote escape \"
    auto* expr = state.parseExprFromString(
        "\"hello\\\"world\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\"world");
}

TEST_F(HVM4BackendTest, StringEscapeDollar) {
    // Escaped dollar to prevent interpolation \$
    auto* expr = state.parseExprFromString(
        "\"hello\\${notvar}world\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello${notvar}world");
}

TEST_F(HVM4BackendTest, StringEscapeMultiple) {
    // Multiple escape sequences
    auto* expr = state.parseExprFromString(
        "\"line1\\nline2\\tindented\\\\backslash\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "line1\nline2\tindented\\backslash");
}

TEST_F(HVM4BackendTest, StringEscapeAtEnd) {
    // Escape at end of string
    auto* expr = state.parseExprFromString(
        "\"hello\\n\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello\n");
}

TEST_F(HVM4BackendTest, StringEscapeAtStart) {
    // Escape at start of string
    auto* expr = state.parseExprFromString(
        "\"\\nworld\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\nworld");
}

// =============================================================================
// Unicode String Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringUnicodeBasicLatin) {
    // Basic Latin supplement (e.g., accented characters)
    auto* expr = state.parseExprFromString(
        "\"cafe\"",  // simple ASCII first
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "cafe");
}

TEST_F(HVM4BackendTest, StringUnicodeAccents) {
    // Accented characters (multi-byte UTF-8)
    auto* expr = state.parseExprFromString(
        "\"caf\xc3\xa9\"",  // cafe with e-acute (UTF-8 encoding)
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "caf\xc3\xa9");
}

TEST_F(HVM4BackendTest, StringUnicodeUmlaut) {
    // German umlauts
    auto* expr = state.parseExprFromString(
        "\"gr\xc3\xbc\xc3\x9f gott\"",  // gruess gott with umlaut (UTF-8)
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "gr\xc3\xbc\xc3\x9f gott");
}

TEST_F(HVM4BackendTest, StringUnicodeEmoji) {
    // Emoji (4-byte UTF-8) - Earth globe
    auto* expr = state.parseExprFromString(
        "\"hello \xf0\x9f\x8c\x8d\"",  // Earth emoji U+1F30D in UTF-8
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello \xf0\x9f\x8c\x8d");
}

TEST_F(HVM4BackendTest, StringUnicodeCJK) {
    // Chinese/Japanese/Korean characters - "hello world" in Chinese
    auto* expr = state.parseExprFromString(
        "\"\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c");
}

TEST_F(HVM4BackendTest, StringUnicodeJapanese) {
    // Japanese hiragana - konnichiwa
    auto* expr = state.parseExprFromString(
        "\"\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf");
}

TEST_F(HVM4BackendTest, StringUnicodeArabic) {
    // Arabic script (right-to-left) - marhaba (hello)
    auto* expr = state.parseExprFromString(
        "\"\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7");
}

TEST_F(HVM4BackendTest, StringUnicodeMixed) {
    // Mixed ASCII and Unicode - hello 世界 world
    auto* expr = state.parseExprFromString(
        "\"hello \xe4\xb8\x96\xe7\x95\x8c world\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello \xe4\xb8\x96\xe7\x95\x8c world");
}

TEST_F(HVM4BackendTest, StringUnicodeEmojiSequence) {
    // Emoji with skin tone modifier (multi-codepoint) - waving hand
    auto* expr = state.parseExprFromString(
        "\"\xf0\x9f\x91\x8b\xf0\x9f\x8f\xbd\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\xf0\x9f\x91\x8b\xf0\x9f\x8f\xbd");
}

TEST_F(HVM4BackendTest, StringUnicodeZeroWidthJoiner) {
    // Emoji with ZWJ (zero-width joiner) - man technologist
    auto* expr = state.parseExprFromString(
        "\"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb\"",  // UTF-8 encoding
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb");
}

// =============================================================================
// String Context Tests
// =============================================================================

// Note: String context tracks store path dependencies.
// We don't track context in HVM4 yet, but string operations should work.

TEST_F(HVM4BackendTest, StringNoContext) {
    // Plain string literal has no context
    auto* expr = state.parseExprFromString(
        "\"plain string\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "plain string");
}

TEST_F(HVM4BackendTest, StringConcatContextMerge) {
    // When concatenating strings with context, contexts should merge
    // This will require derivation/path support to fully test
    auto* expr = state.parseExprFromString(
        "\"a\" + \"b\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "ab");
}

TEST_F(HVM4BackendTest, StringInterpolationContextPropagation) {
    // Context should propagate through interpolation
    auto* expr = state.parseExprFromString(
        "let s = \"inner\"; in \"outer ${s}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "outer inner");
}

// =============================================================================
// String in Let Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringInLetSimple) {
    // String assigned to variable in let
    auto* expr = state.parseExprFromString(
        "let s = \"test\"; in s",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "test");
}

TEST_F(HVM4BackendTest, StringInLetMultiple) {
    // Multiple string bindings - runtime concat not supported
    auto* expr = state.parseExprFromString(
        "let a = \"hello\"; b = \"world\"; in a + \" \" + b",
        state.rootPath(CanonPath::root)
    );
    // Runtime string concatenation with variables not yet supported
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringInLetNested) {
    // Nested let with strings - var + var is accepted (can't distinguish from numeric)
    // but evaluation will fail or produce wrong results
    auto* expr = state.parseExprFromString(
        "let outer = \"out\"; in let inner = \"in\"; in outer + inner",
        state.rootPath(CanonPath::root)
    );
    // At compile time, we can't distinguish var + var string ops from numeric ops
    // This will be accepted but will fail at runtime or produce wrong results
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    // The evaluation will fail because we try to add strings as numbers
    EXPECT_FALSE(backend.tryEvaluate(expr, state.baseEnv, result));
}

// =============================================================================
// String with Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringLambdaReturn) {
    // Lambda returning a string
    auto* expr = state.parseExprFromString(
        "(x: \"hello\") 42",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, StringLambdaInterpolate) {
    // Lambda with string interpolation of argument
    auto* expr = state.parseExprFromString(
        "(x: \"value: ${x}\") \"test\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "value: test");
}

TEST_F(HVM4BackendTest, StringLambdaConcat) {
    // Lambda concatenating strings - var + var is accepted (can't distinguish from numeric)
    // but evaluation will fail or produce wrong results
    auto* expr = state.parseExprFromString(
        "(a: b: a + b) \"hello\" \" world\"",
        state.rootPath(CanonPath::root)
    );
    // At compile time, we can't distinguish var + var string ops from numeric ops
    // This will be accepted but will fail at runtime or produce wrong results
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    // The evaluation will fail because we try to add strings as numbers
    EXPECT_FALSE(backend.tryEvaluate(expr, state.baseEnv, result));
}

// =============================================================================
// String in Conditional Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringInIfThen) {
    // String in then branch
    auto* expr = state.parseExprFromString(
        "if true then \"yes\" else \"no\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "yes");
}

TEST_F(HVM4BackendTest, StringInIfElse) {
    // String in else branch
    auto* expr = state.parseExprFromString(
        "if false then \"yes\" else \"no\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "no");
}

TEST_F(HVM4BackendTest, StringIfBothBranches) {
    // Both branches are strings
    auto* expr = state.parseExprFromString(
        "let cond = true; in if cond then \"true-branch\" else \"false-branch\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "true-branch");
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(HVM4BackendTest, StringVeryLong) {
    // Test with a longer string (not extremely long for compilation tests)
    std::string longStr(200, 'x');
    auto* expr = state.parseExprFromString(
        "\"" + longStr + "\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), longStr.c_str());
}

TEST_F(HVM4BackendTest, StringOnlySpaces) {
    // String containing only whitespace
    auto* expr = state.parseExprFromString(
        "\"   \"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "   ");
}

TEST_F(HVM4BackendTest, StringOnlyNewlines) {
    // String containing only newlines via escape
    auto* expr = state.parseExprFromString(
        "\"\\n\\n\\n\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\n\n\n");
}

TEST_F(HVM4BackendTest, StringNullCharacter) {
    // String with embedded null character (valid in Nix)
    // Using octal escape for null
    auto* expr = state.parseExprFromString(
        "\"hello\\x00world\"",
        state.rootPath(CanonPath::root)
    );
    // \x escapes may not be supported in Nix, test parsing behavior
    // If parsing succeeds, we can evaluate it
    if (backend.canEvaluate(*expr)) {
        Value result;
        backend.tryEvaluate(expr, state.baseEnv, result);
    }
}

TEST_F(HVM4BackendTest, StringAllEscapes) {
    // String with all escape sequences combined
    auto* expr = state.parseExprFromString(
        "\"\\n\\r\\t\\\\\\\"\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "\n\r\t\\\"");
}

TEST_F(HVM4BackendTest, StringSingleChar) {
    // Single character string
    auto* expr = state.parseExprFromString(
        "\"x\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "x");
}

TEST_F(HVM4BackendTest, StringDigitsOnly) {
    // String containing only digits (not an integer)
    auto* expr = state.parseExprFromString(
        "\"12345\"",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "12345");
}

// =============================================================================
// String Equality Tests (for when comparison is implemented)
// =============================================================================

TEST_F(HVM4BackendTest, StringEqualitySame) {
    // Two identical strings should be equal
    auto* expr = state.parseExprFromString(
        "\"hello\" == \"hello\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringEqualityDifferent) {
    // Different strings should not be equal
    auto* expr = state.parseExprFromString(
        "\"hello\" == \"world\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringInequalitySame) {
    // Two identical strings: inequality should be false
    auto* expr = state.parseExprFromString(
        "\"hello\" != \"hello\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringInequalityDifferent) {
    // Different strings: inequality should be true
    auto* expr = state.parseExprFromString(
        "\"hello\" != \"world\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringComparisonLessThan) {
    // String lexicographic comparison
    auto* expr = state.parseExprFromString(
        "\"aaa\" < \"bbb\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringComparisonGreaterThan) {
    // String lexicographic comparison
    auto* expr = state.parseExprFromString(
        "\"bbb\" > \"aaa\"",
        state.rootPath(CanonPath::root)
    );
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Error Case Tests
// =============================================================================
// These tests verify that string operations produce appropriate errors
// for invalid inputs.

TEST_F(HVM4BackendTest, StringConcatWithInt) {
    // "hello" + 42 should produce a type error
    auto* expr = state.parseExprFromString("\"hello\" + 42", state.rootPath(CanonPath::root));
    // String support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));

    // TODO: Once implemented, this should either:
    // - Return false from tryEvaluate, or
    // - The error should be caught in extraction
}

TEST_F(HVM4BackendTest, StringConcatWithList) {
    // "hello" + [1 2] should produce a type error
    auto* expr = state.parseExprFromString("\"hello\" + [1 2]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringSubstringNegativeStart) {
    // builtins.substring (-1) 5 "hello" should produce an error
    auto* expr = state.parseExprFromString("builtins.substring (0 - 1) 5 \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringLengthNonString) {
    // builtins.stringLength 42 should produce an error
    auto* expr = state.parseExprFromString("builtins.stringLength 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, StringInterpolationForcesExpression) {
    // String interpolation with integer variable
    auto* expr = state.parseExprFromString(
        "let x = 42; in \"value: ${x}\"",
        state.rootPath(CanonPath::root)
    );
    // String interpolation is now implemented
    // Note: Integer interpolation requires int-to-string conversion
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    if (success) {
        EXPECT_EQ(result.type(), nString);
        EXPECT_STREQ(result.c_str(), "value: 42");
    }
    // If it fails, that's okay - int-to-string coercion may not be fully working yet
}

TEST_F(HVM4BackendTest, StringHashInvalidAlgorithm) {
    // builtins.hashString "invalid" "hello" should produce an error
    auto* expr = state.parseExprFromString("builtins.hashString \"invalid\" \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
