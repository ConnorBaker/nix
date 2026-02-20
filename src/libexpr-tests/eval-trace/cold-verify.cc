/**
 * Tests for cold verification utilities: deepCompare() and verifyCold().
 *
 * deepCompare() recursively compares two Nix values and returns a diagnostic
 * string on the first mismatch, or std::nullopt if they match.
 *
 * verifyCold() performs a complete second evaluation with dependency tracking
 * disabled and compares the result against the traced evaluation.
 */

#include "helpers.hh"

#include "nix/expr/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value/context.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── deepCompare positive tests (values match → nullopt) ─────────────

class DeepCompareTest : public EvalTraceTest {};

TEST_F(DeepCompareTest, MatchingIntegers)
{
    Value a = eval("42");
    Value b = eval("42");
    auto result = deepCompare(state, a, b, "test");
    EXPECT_FALSE(result.has_value());
}

TEST_F(DeepCompareTest, MatchingStrings)
{
    Value a = eval("\"hello world\"");
    Value b = eval("\"hello world\"");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingEmptyStrings)
{
    Value a = eval("\"\"");
    Value b = eval("\"\"");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingBoolTrue)
{
    Value a = eval("true");
    Value b = eval("true");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingBoolFalse)
{
    Value a = eval("false");
    Value b = eval("false");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingNull)
{
    Value a = eval("null");
    Value b = eval("null");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingFloats)
{
    Value a = eval("3.14");
    Value b = eval("3.14");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingPaths)
{
    Value a = eval("/tmp");
    Value b = eval("/tmp");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingSimpleAttrset)
{
    Value a = eval("{ x = 1; y = 2; }");
    Value b = eval("{ x = 1; y = 2; }");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingNestedAttrset)
{
    Value a = eval("{ a = { b = { c = 42; }; }; }");
    Value b = eval("{ a = { b = { c = 42; }; }; }");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingEmptyAttrset)
{
    Value a = eval("{ }");
    Value b = eval("{ }");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingSimpleList)
{
    Value a = eval("[ 1 2 3 ]");
    Value b = eval("[ 1 2 3 ]");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingNestedList)
{
    Value a = eval("[ [ 1 2 ] [ 3 4 ] ]");
    Value b = eval("[ [ 1 2 ] [ 3 4 ] ]");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingEmptyList)
{
    Value a = eval("[ ]");
    Value b = eval("[ ]");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, MatchingMixedAttrset)
{
    Value a = eval("{ name = \"hello\"; version = 1; flag = true; nothing = null; }");
    Value b = eval("{ name = \"hello\"; version = 1; flag = true; nothing = null; }");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, FunctionsSkipped)
{
    // Functions can't be compared — deepCompare returns nullopt (match)
    Value a = eval("x: x + 1");
    Value b = eval("y: y + 2");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

TEST_F(DeepCompareTest, DepthLimitReturnsMatch)
{
    // At depth > 20, deepCompare gives up and returns nullopt
    Value a = eval("1");
    Value b = eval("2");
    // depth=21 should skip comparison
    EXPECT_FALSE(deepCompare(state, a, b, "test", 21).has_value());
}

TEST_F(DeepCompareTest, MatchingStringWithContext)
{
    // Strings with context from string interpolation of paths
    Value a = eval(R"("prefix-${builtins.toFile "ctx-test" "content"}-suffix")");
    Value b = eval(R"("prefix-${builtins.toFile "ctx-test" "content"}-suffix")");
    EXPECT_FALSE(deepCompare(state, a, b, "test").has_value());
}

// ── deepCompare negative tests (values differ → diagnostic string) ──

TEST_F(DeepCompareTest, TypeMismatch_IntVsString)
{
    Value a = eval("42");
    Value b = eval("\"42\"");
    auto result = deepCompare(state, a, b, "test");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("type mismatch"), std::string::npos);
    EXPECT_NE(result->find("test"), std::string::npos);
}

TEST_F(DeepCompareTest, TypeMismatch_BoolVsInt)
{
    Value a = eval("true");
    Value b = eval("1");
    auto result = deepCompare(state, a, b, "test");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("type mismatch"), std::string::npos);
}

TEST_F(DeepCompareTest, TypeMismatch_NullVsString)
{
    Value a = eval("null");
    Value b = eval("\"null\"");
    auto result = deepCompare(state, a, b, "test");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("type mismatch"), std::string::npos);
}

TEST_F(DeepCompareTest, TypeMismatch_AttrsetVsList)
{
    Value a = eval("{ }");
    Value b = eval("[ ]");
    auto result = deepCompare(state, a, b, "test");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("type mismatch"), std::string::npos);
}

TEST_F(DeepCompareTest, IntMismatch)
{
    Value a = eval("42");
    Value b = eval("43");
    auto result = deepCompare(state, a, b, "root");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("int mismatch"), std::string::npos);
    EXPECT_NE(result->find("root"), std::string::npos);
    EXPECT_NE(result->find("42"), std::string::npos);
    EXPECT_NE(result->find("43"), std::string::npos);
}

TEST_F(DeepCompareTest, FloatMismatch)
{
    Value a = eval("3.14");
    Value b = eval("2.71");
    auto result = deepCompare(state, a, b, "pi");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("float mismatch"), std::string::npos);
    EXPECT_NE(result->find("pi"), std::string::npos);
}

TEST_F(DeepCompareTest, BoolMismatch)
{
    Value a = eval("true");
    Value b = eval("false");
    auto result = deepCompare(state, a, b, "flag");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("bool mismatch"), std::string::npos);
    EXPECT_NE(result->find("flag"), std::string::npos);
    EXPECT_NE(result->find("true"), std::string::npos);
    EXPECT_NE(result->find("false"), std::string::npos);
}

TEST_F(DeepCompareTest, PathMismatch)
{
    Value a = eval("/tmp/a");
    Value b = eval("/tmp/b");
    auto result = deepCompare(state, a, b, "src");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("path mismatch"), std::string::npos);
    EXPECT_NE(result->find("/tmp/a"), std::string::npos);
    EXPECT_NE(result->find("/tmp/b"), std::string::npos);
}

TEST_F(DeepCompareTest, StringMismatch_ShowsPosition)
{
    Value a = eval("\"hello world\"");
    Value b = eval("\"hello globe\"");
    auto result = deepCompare(state, a, b, "greeting");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("string mismatch"), std::string::npos);
    EXPECT_NE(result->find("greeting"), std::string::npos);
    // Should include position info
    EXPECT_NE(result->find("pos"), std::string::npos);
}

TEST_F(DeepCompareTest, StringValueDiffFromInterpolation)
{
    // Different toFile names produce different store paths in the interpolated string,
    // so the string values themselves differ (caught before context comparison)
    Value a = eval(R"("hello-${builtins.toFile "ctx-a" "content-a"}")");
    Value b = eval(R"("hello-${builtins.toFile "ctx-b" "content-b"}")");
    auto result = deepCompare(state, a, b, "ctx");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("string mismatch"), std::string::npos);
    EXPECT_NE(result->find("ctx"), std::string::npos);
}

TEST_F(DeepCompareTest, StringContextOnlyMismatch)
{
    // Same string text, different contexts — tests the context comparison path
    NixStringContext ctxA, ctxB;
    ctxA.insert(NixStringContextElem::parse("g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo"));
    ctxB.insert(NixStringContextElem::parse("g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"));

    Value a, b;
    a.mkString("same-text", ctxA, state.mem);
    b.mkString("same-text", ctxB, state.mem);

    auto result = deepCompare(state, a, b, "ctx");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("string context mismatch"), std::string::npos);
    EXPECT_NE(result->find("ctx"), std::string::npos);
    // Should mention which context elements differ
    EXPECT_NE(result->find("foo"), std::string::npos);
    EXPECT_NE(result->find("bar"), std::string::npos);
}

TEST_F(DeepCompareTest, AttrsetKeyMismatch_ExtraKey)
{
    Value a = eval("{ x = 1; y = 2; }");
    Value b = eval("{ x = 1; z = 3; }");
    auto result = deepCompare(state, a, b, "pkg");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("attrset key mismatch"), std::string::npos);
    EXPECT_NE(result->find("pkg"), std::string::npos);
    // Should mention the differing keys
    EXPECT_NE(result->find("y"), std::string::npos);
    EXPECT_NE(result->find("z"), std::string::npos);
}

TEST_F(DeepCompareTest, AttrsetKeyMismatch_MissingKey)
{
    Value a = eval("{ x = 1; y = 2; }");
    Value b = eval("{ x = 1; }");
    auto result = deepCompare(state, a, b, "root");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("attrset key mismatch"), std::string::npos);
    EXPECT_NE(result->find("y"), std::string::npos);
}

TEST_F(DeepCompareTest, AttrsetNestedValueMismatch)
{
    Value a = eval("{ outer = { inner = 1; }; }");
    Value b = eval("{ outer = { inner = 2; }; }");
    auto result = deepCompare(state, a, b, "root");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("int mismatch"), std::string::npos);
    // Path should include both levels
    EXPECT_NE(result->find("outer"), std::string::npos);
    EXPECT_NE(result->find("inner"), std::string::npos);
}

TEST_F(DeepCompareTest, AttrsetDeeplyNestedMismatch)
{
    Value a = eval("{ a = { b = { c = { d = 10; }; }; }; }");
    Value b = eval("{ a = { b = { c = { d = 20; }; }; }; }");
    auto result = deepCompare(state, a, b, "root");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("int mismatch"), std::string::npos);
    EXPECT_NE(result->find("d"), std::string::npos);
}

TEST_F(DeepCompareTest, ListSizeMismatch)
{
    Value a = eval("[ 1 2 3 ]");
    Value b = eval("[ 1 2 ]");
    auto result = deepCompare(state, a, b, "items");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("list size mismatch"), std::string::npos);
    EXPECT_NE(result->find("items"), std::string::npos);
    EXPECT_NE(result->find("3"), std::string::npos);
    EXPECT_NE(result->find("2"), std::string::npos);
}

TEST_F(DeepCompareTest, ListElementMismatch)
{
    Value a = eval("[ 1 2 3 ]");
    Value b = eval("[ 1 99 3 ]");
    auto result = deepCompare(state, a, b, "items");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("int mismatch"), std::string::npos);
    // Path should include index
    EXPECT_NE(result->find("[1]"), std::string::npos);
}

TEST_F(DeepCompareTest, ListNestedAttrsetMismatch)
{
    Value a = eval(R"([ { name = "foo"; } { name = "bar"; } ])");
    Value b = eval(R"([ { name = "foo"; } { name = "baz"; } ])");
    auto result = deepCompare(state, a, b, "list");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("string mismatch"), std::string::npos);
    EXPECT_NE(result->find("[1]"), std::string::npos);
    EXPECT_NE(result->find("name"), std::string::npos);
}

TEST_F(DeepCompareTest, EmptyPathPrefix)
{
    // When path is empty, child paths should just be the attr name
    Value a = eval("{ x = 1; }");
    Value b = eval("{ x = 2; }");
    auto result = deepCompare(state, a, b, "");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("int mismatch"), std::string::npos);
    // Path should be just "x", not ".x"
    EXPECT_NE(result->find("'x'"), std::string::npos);
}

TEST_F(DeepCompareTest, MismatchReportsFirstDifference)
{
    // deepCompare should stop at the first mismatch
    Value a = eval("{ x = 1; y = \"a\"; z = true; }");
    Value b = eval("{ x = 2; y = \"b\"; z = false; }");
    auto result = deepCompare(state, a, b, "root");
    ASSERT_TRUE(result.has_value());
    // Only one mismatch reported (the first found)
    // Count "mismatch" occurrences — should be exactly 1
    size_t count = 0;
    size_t pos = 0;
    while ((pos = result->find("mismatch", pos)) != std::string::npos) {
        count++;
        pos += 8;
    }
    EXPECT_EQ(count, 1u);
}

// ── verifyCold positive tests (matching → no throw) ─────────────────

class VerifyColdTest : public TraceCacheFixture {};

TEST_F(VerifyColdTest, MatchingInteger)
{
    auto cache = makeCache("{ x = 42; }");
    auto root = forceRoot(*cache);
    auto * xAttr = root.attrs()->get(state.symbols.create("x"));
    state.forceValue(*xAttr->value, noPos);
    // verifyCold should not throw when traced and cold values match
    EXPECT_NO_THROW(cache->verifyCold("x", *xAttr->value));
}

TEST_F(VerifyColdTest, MatchingString)
{
    auto cache = makeCache("{ greeting = \"hello\"; }");
    auto root = forceRoot(*cache);
    auto * attr = root.attrs()->get(state.symbols.create("greeting"));
    state.forceValue(*attr->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("greeting", *attr->value));
}

TEST_F(VerifyColdTest, MatchingNestedAttr)
{
    auto cache = makeCache("{ a = { b = 99; }; }");
    auto root = forceRoot(*cache);
    auto * a = root.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    auto * b = a->value->attrs()->get(state.symbols.create("b"));
    state.forceValue(*b->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("a.b", *b->value));
}

TEST_F(VerifyColdTest, MatchingAttrset)
{
    auto cache = makeCache("{ pkg = { name = \"hello\"; version = \"1.0\"; }; }");
    auto root = forceRoot(*cache);
    auto * pkg = root.attrs()->get(state.symbols.create("pkg"));
    state.forceValue(*pkg->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("pkg", *pkg->value));
}

TEST_F(VerifyColdTest, MatchingList)
{
    auto cache = makeCache("{ items = [ 1 2 3 ]; }");
    auto root = forceRoot(*cache);
    auto * attr = root.attrs()->get(state.symbols.create("items"));
    state.forceValue(*attr->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("items", *attr->value));
}

TEST_F(VerifyColdTest, MatchingBool)
{
    auto cache = makeCache("{ flag = true; }");
    auto root = forceRoot(*cache);
    auto * attr = root.attrs()->get(state.symbols.create("flag"));
    state.forceValue(*attr->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("flag", *attr->value));
}

TEST_F(VerifyColdTest, MatchingNull)
{
    auto cache = makeCache("{ nothing = null; }");
    auto root = forceRoot(*cache);
    auto * attr = root.attrs()->get(state.symbols.create("nothing"));
    state.forceValue(*attr->value, noPos);
    EXPECT_NO_THROW(cache->verifyCold("nothing", *attr->value));
}

// ── verifyCold negative tests (mismatch → throws Error) ─────────────

TEST_F(VerifyColdTest, MismatchInteger)
{
    auto cache = makeCache("{ x = 42; }");
    // Create a different value to pass as "traced result"
    Value wrong = eval("99");
    EXPECT_THROW(cache->verifyCold("x", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchString)
{
    auto cache = makeCache("{ name = \"hello\"; }");
    Value wrong = eval("\"world\"");
    EXPECT_THROW(cache->verifyCold("name", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchType)
{
    auto cache = makeCache("{ x = 42; }");
    Value wrong = eval("\"42\"");
    EXPECT_THROW(cache->verifyCold("x", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchNestedValue)
{
    auto cache = makeCache("{ a = { b = 1; }; }");
    Value wrong = eval("{ b = 2; }");
    EXPECT_THROW(cache->verifyCold("a", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchAttrsetKeys)
{
    auto cache = makeCache("{ pkg = { x = 1; y = 2; }; }");
    Value wrong = eval("{ x = 1; z = 3; }");
    EXPECT_THROW(cache->verifyCold("pkg", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchListSize)
{
    auto cache = makeCache("{ items = [ 1 2 3 ]; }");
    Value wrong = eval("[ 1 2 ]");
    EXPECT_THROW(cache->verifyCold("items", wrong), Error);
}

TEST_F(VerifyColdTest, MismatchListElement)
{
    auto cache = makeCache("{ items = [ 1 2 3 ]; }");
    Value wrong = eval("[ 1 99 3 ]");
    EXPECT_THROW(cache->verifyCold("items", wrong), Error);
}

TEST_F(VerifyColdTest, ErrorMessageIncludesDiagnostics)
{
    auto cache = makeCache("{ x = 42; }");
    Value wrong = eval("99");
    try {
        cache->verifyCold("x", wrong);
        FAIL() << "Expected Error to be thrown";
    } catch (Error & e) {
        auto msg = e.info().msg.str();
        EXPECT_NE(msg.find("verify-eval-trace (cold)"), std::string::npos);
        EXPECT_NE(msg.find("int mismatch"), std::string::npos);
    }
}

TEST_F(VerifyColdTest, InvalidAttrPathThrows)
{
    auto cache = makeCache("{ x = 42; }");
    Value v = eval("42");
    // Navigating to a nonexistent attribute should throw
    EXPECT_THROW(cache->verifyCold("nonexistent", v), Error);
}

} // namespace nix::eval_trace::test
