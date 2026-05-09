/// show-for-hash.cc — Tests for Expr::showForHash path normalization.
///
/// showForHash mirrors show() but normalizes ExprPath output by stripping
/// basePath via CanonPath::removePrefix. This ensures NixBinding scope and
/// binding hashes are stable across accessor types (store paths vs local).

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/nixexpr.hh"

#include <gtest/gtest.h>
#include <sstream>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class ShowForHashTest : public EvalTraceTest {};

// ── showForHash matches show for non-path expressions ────────────────

TEST_F(ShowForHashTest, Hash_NonPathExpr_MatchesShow)
{
    // For expressions without path literals, showForHash and show
    // should produce identical output.
    std::vector<std::string> exprs = {
        "42",
        "\"hello\"",
        "true",
        "x: x + 1",
        "{ a = 1; b = 2; }",
        "let x = 1; in x + 2",
        "with builtins; length [1 2 3]",
        "if true then 1 else 2",
        "assert true; 42",
        "! false",
        "(1 + 2)",
        "[ 1 \"two\" (3 + 4) ]",
    };

    auto basePath = CanonPath("/some/base/path");

    for (auto & src : exprs) {
        auto ast = state.parseExprFromString(src, state.rootPath(CanonPath("/test.nix")));

        std::ostringstream showOut, hashOut;
        ast->show(state.symbols, showOut);
        ast->showForHash(state.symbols, hashOut, basePath);

        EXPECT_EQ(showOut.str(), hashOut.str())
            << "showForHash should match show for non-path expression: " << src;
    }
}

// ── showForHash normalizes ExprPath relative to basePath ─────────────

TEST_F(ShowForHashTest, Hash_PathExpr_NormalizedByBasePath)
{
    // An expression with a path literal: import ./lib/helper.nix
    // When the basePath matches the directory containing the path,
    // showForHash strips the prefix to produce a relative path.
    std::string src = "import ./lib/helper.nix";
    auto basePath = CanonPath("/nix/store/abc123-source");

    auto ast = state.parseExprFromString(
        src, state.rootPath(CanonPath("/nix/store/abc123-source/flake.nix")));

    std::ostringstream showOut, hashOut;
    ast->show(state.symbols, showOut);
    ast->showForHash(state.symbols, hashOut, basePath);

    // show() produces the absolute store path
    auto showStr = showOut.str();
    EXPECT_TRUE(showStr.find("/nix/store/abc123-source") != std::string::npos)
        << "show() should contain absolute store path, got: " << showStr;

    // showForHash() should strip the base path prefix
    auto hashStr = hashOut.str();
    EXPECT_TRUE(hashStr.find("/nix/store/abc123-source") == std::string::npos)
        << "showForHash() should NOT contain absolute store path, got: " << hashStr;
    EXPECT_TRUE(hashStr.find("/lib/helper.nix") != std::string::npos)
        << "showForHash() should contain relative path, got: " << hashStr;
}

// ── Same code, different store paths → same showForHash output ───────

TEST_F(ShowForHashTest, Hash_DifferentStorePaths_SameNormalizedOutput)
{
    // The same flake.nix at two different store paths should produce
    // identical showForHash output (and thus identical binding hashes).
    // This is the core property that makes NixBinding hashes stable
    // across flake input version changes.
    std::string src = "import ./lib/helper.nix";

    auto storePath1 = CanonPath("/nix/store/abc123-source");
    auto storePath2 = CanonPath("/nix/store/def456-source");

    auto ast1 = state.parseExprFromString(
        src, state.rootPath(storePath1 / "flake.nix"));
    auto ast2 = state.parseExprFromString(
        src, state.rootPath(storePath2 / "flake.nix"));

    std::ostringstream show1, show2, hash1, hash2;
    ast1->show(state.symbols, show1);
    ast2->show(state.symbols, show2);
    ast1->showForHash(state.symbols, hash1, storePath1);
    ast2->showForHash(state.symbols, hash2, storePath2);

    // show() differs (different absolute paths)
    EXPECT_NE(show1.str(), show2.str())
        << "show() should differ for different store paths";

    // showForHash() is identical (relative paths)
    EXPECT_EQ(hash1.str(), hash2.str())
        << "showForHash() should be identical for same code at different store paths"
        << "\n  hash1: " << hash1.str()
        << "\n  hash2: " << hash2.str();
}

// ── Round-trip: NixBinding hashes stable via showForHash ─────────────

TEST_F(ShowForHashTest, Hash_NixBindingHash_StableAcrossStorePaths)
{
    // Full round-trip: compute NixBinding scope + binding hashes for
    // the same flake.nix at two different store paths. Hashes must match.
    std::string nixCode = R"(
        let
            helper = import ./lib/helper.nix;
        in {
            foo = helper.value;
            bar = "hello";
        }
    )";

    auto storePath1 = CanonPath("/nix/store/abc123-source");
    auto storePath2 = CanonPath("/nix/store/def456-source");

    auto ast1 = state.parseExprFromString(nixCode, state.rootPath(storePath1 / "flake.nix"));
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols, &storePath1);

    auto ast2 = state.parseExprFromString(nixCode, state.rootPath(storePath2 / "flake.nix"));
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols, &storePath2);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash should be stable across store paths"
        << "\n  hash1=" << scopeHash1.value.toHex()
        << "\n  hash2=" << scopeHash2.value.toHex();

    // Check per-binding hashes
    std::map<std::string, NixBindingHash> hashes1, hashes2;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind), def.e, state.symbols, &storePath1);
    }
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind), def.e, state.symbols, &storePath2);
    }

    ASSERT_EQ(hashes1.size(), hashes2.size());
    for (auto & [name, h1] : hashes1) {
        auto it = hashes2.find(name);
        ASSERT_NE(it, hashes2.end());
        EXPECT_EQ(h1, it->second)
            << "Binding '" << name << "' hash should be stable across store paths"
            << "\n  hash1=" << h1.value.toHex()
            << "\n  hash2=" << it->second.value.toHex();
    }
}

// ── showForHash for path outside basePath → unchanged ────────────────

TEST_F(ShowForHashTest, Hash_PathOutsideBasePath_Unchanged)
{
    // A path literal that points outside the basePath should be
    // left unchanged by showForHash (no prefix to strip).
    std::string src = "import /absolute/other/path.nix";

    auto basePath = CanonPath("/nix/store/abc123-source");
    auto ast = state.parseExprFromString(src, state.rootPath(CanonPath("/test.nix")));

    std::ostringstream showOut, hashOut;
    ast->show(state.symbols, showOut);
    ast->showForHash(state.symbols, hashOut, basePath);

    EXPECT_EQ(showOut.str(), hashOut.str())
        << "Path outside basePath should be unchanged by showForHash";
}

// ── Negative: changing binding content changes hash ──────────────────

TEST_F(ShowForHashTest, Hash_DifferentBindingContent_DifferentHash)
{
    std::string code1 = R"({ foo = 1; bar = 2; })";
    std::string code2 = R"({ foo = 1; bar = 3; })";

    auto basePath = CanonPath("/test");

    auto ast1 = state.parseExprFromString(code1, state.rootPath(CanonPath("/test/file.nix")));
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols, &basePath);

    auto ast2 = state.parseExprFromString(code2, state.rootPath(CanonPath("/test/file.nix")));
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols, &basePath);

    // Scope hashes should be equal (same scope structure, no let/lambda)
    EXPECT_EQ(scopeHash1, scopeHash2);

    // But the "bar" binding hash should differ
    auto getHash = [&](ExprAttrs * attrs, const NixScopeHash & scope, const CanonPath & bp, const std::string & name) {
        for (auto & [sym, def] : *attrs->attrs) {
            if (std::string(state.symbols[sym]) == name)
                return computeNixBindingHash(scope, name, static_cast<int>(def.kind), def.e, state.symbols, &bp);
        }
        throw Error("binding not found: %s", name);
    };

    EXPECT_EQ(getHash(attrs1, scopeHash1, basePath, "foo"),
              getHash(attrs2, scopeHash2, basePath, "foo"))
        << "foo binding unchanged → same hash";

    EXPECT_NE(getHash(attrs1, scopeHash1, basePath, "bar"),
              getHash(attrs2, scopeHash2, basePath, "bar"))
        << "bar binding changed → different hash";
}

} // namespace nix::eval_trace
