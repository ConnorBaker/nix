/// Unit tests for the expression generators in expr-gen.cc.
///
/// These tests verify that each generator produces well-formed TestExpr values
/// without running a full property loop against the eval-trace cache.  No
/// EvalState or TraceSession is needed — generators only create temp files and
/// construct TestExpr structs.

#include "expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test::proptest;

static rc::detail::TestParams makeParams()
{
    auto params = rc::detail::configuration().testParams;
    params.maxSuccess = 50;
    return params;
}

// ── ScalarGen ────────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, ScalarGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeScalarGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.empty());
            RC_ASSERT(
                expr.expectedKind == TestExpr::ResultKind::Int ||
                expr.expectedKind == TestExpr::ResultKind::String ||
                expr.expectedKind == TestExpr::ResultKind::Bool ||
                expr.expectedKind == TestExpr::ResultKind::Null ||
                expr.expectedKind == TestExpr::ResultKind::Float);
        },
        makeParams);
}

// ── ReadFileGen ──────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, ReadFileGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeReadFileGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::File);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            RC_ASSERT(expr.expectedKind == TestExpr::ResultKind::String);
            // nixCode must reference the file path.
            RC_ASSERT(expr.nixCode.find("readFile") != std::string::npos);
        },
        makeParams);
}

// ── GetEnvGen ────────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, GetEnvGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeGetEnvGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::EnvVar);
            RC_ASSERT(!expr.depSlots[0].envVarName.empty());
            RC_ASSERT(expr.expectedKind == TestExpr::ResultKind::String);
            RC_ASSERT(expr.nixCode.find("getEnv") != std::string::npos);
        },
        makeParams);
}

// ── FromJSONGen ──────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, FromJSONGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeFromJSONGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::JsonFile);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            RC_ASSERT(expr.expectedKind == TestExpr::ResultKind::Attrset);
            // nixCode must contain both builtins.
            RC_ASSERT(expr.nixCode.find("fromJSON") != std::string::npos);
            RC_ASSERT(expr.nixCode.find("readFile") != std::string::npos);
        },
        makeParams);
}

// ── AttrAccessGen ────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, AttrAccessGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeAttrAccessGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::JsonFile);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            // ResultKind is one of the JSON scalar kinds (not Attrset).
            RC_ASSERT(
                expr.expectedKind == TestExpr::ResultKind::String ||
                expr.expectedKind == TestExpr::ResultKind::Int ||
                expr.expectedKind == TestExpr::ResultKind::Bool ||
                expr.expectedKind == TestExpr::ResultKind::Null);
            // nixCode must contain a dot-access pattern.
            RC_ASSERT(expr.nixCode.find("fromJSON") != std::string::npos);
            RC_ASSERT(expr.nixCode.find("readFile") != std::string::npos);
            // Attribute access uses quoted key syntax: )."<key>"
            RC_ASSERT(expr.nixCode.find(").\"") != std::string::npos);
        },
        makeParams);
}

// ── PathExistsGen ────────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, PathExistsGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makePathExistsGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::FileExistence);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            RC_ASSERT(expr.depSlots[0].currentValue == "exists");
            RC_ASSERT(expr.expectedKind == TestExpr::ResultKind::Bool);
            RC_ASSERT(expr.nixCode.find("pathExists") != std::string::npos);
        },
        makeParams);
}

// ── MultiSourceAttrGen ───────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, MultiSourceAttrGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeMultiSourceAttrGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 2);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::JsonFile);
            RC_ASSERT(expr.depSlots[1].kind == DepSlot::Kind::JsonFile);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            RC_ASSERT(!expr.depSlots[1].path.empty());
            // Two different file paths.
            RC_ASSERT(expr.depSlots[0].path != expr.depSlots[1].path);
            // nixCode must contain // and two readFile calls.
            RC_ASSERT(expr.nixCode.find("//") != std::string::npos);
            RC_ASSERT(expr.nixCode.find("fromJSON") != std::string::npos);
            // ResultKind is a JSON scalar kind.
            RC_ASSERT(
                expr.expectedKind == TestExpr::ResultKind::String ||
                expr.expectedKind == TestExpr::ResultKind::Int ||
                expr.expectedKind == TestExpr::ResultKind::Bool ||
                expr.expectedKind == TestExpr::ResultKind::Null);
        },
        makeParams);
}

// ── ListFromJSONGen ──────────────────────────────────────────────────────────

TEST(EvalTraceProperty_Generator, ListFromJSONGen_WellFormed)
{
    rc::detail::checkGTestWith(
        []() {
            TestExpr expr = *makeListFromJSONGen();
            RC_ASSERT(!expr.nixCode.empty());
            RC_ASSERT(expr.depSlots.size() == 1);
            RC_ASSERT(expr.depSlots[0].kind == DepSlot::Kind::JsonArray);
            RC_ASSERT(!expr.depSlots[0].path.empty());
            RC_ASSERT(expr.expectedKind == TestExpr::ResultKind::List);
            RC_ASSERT(expr.nixCode.find("fromJSON") != std::string::npos);
            RC_ASSERT(expr.nixCode.find("readFile") != std::string::npos);
            // Content must be a valid JSON array.
            auto content = expr.depSlots[0].currentValue;
            RC_ASSERT(content.front() == '[');
            RC_ASSERT(content.back() == ']');
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
