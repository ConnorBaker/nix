/**
 * Tests for verifyCold(): performs a complete second evaluation with dependency
 * tracking disabled and compares the result against the traced evaluation.
 */

#include "eval-trace/helpers.hh"

#include "nix/expr/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value/context.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── verifyCold positive tests (matching -> no throw) ---------------------

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

// ── verifyCold negative tests (mismatch -> throws Error) -----------------

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
