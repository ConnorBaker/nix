#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/nixexpr.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class NixBindingDeterminismTest : public EvalTraceTest {};

// Parse the same nix expression twice and compare NixBinding hashes.
// Both parses use parseExprFromString (same code path as verification).
// If hashes differ, the issue is in show() non-determinism.

TEST_F(NixBindingDeterminismTest, SameStringTwice_SameScopeHash)
{
    std::string nixCode = R"(
        let
            helper = x: x + 1;
        in {
            foo = helper 42;
            bar = "hello";
        }
    )";

    auto basePath = state.rootPath(CanonPath("/test.nix"));

    // Parse 1
    auto ast1 = state.parseExprFromString(nixCode, basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    // Parse 2
    auto ast2 = state.parseExprFromString(nixCode, basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash should be deterministic for the same source code"
        << "\n  hash1=" << scopeHash1.toHex()
        << "\n  hash2=" << scopeHash2.toHex();
}

TEST_F(NixBindingDeterminismTest, SameStringTwice_SameBindingHash)
{
    std::string nixCode = R"(
        let
            helper = x: x + 1;
        in {
            foo = helper 42;
            bar = "hello";
        }
    )";

    auto basePath = state.rootPath(CanonPath("/test.nix"));

    // Parse 1
    auto ast1 = state.parseExprFromString(nixCode, basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    std::map<std::string, Blake3Hash> hashes1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        auto * expr = def.e;
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind), expr, state.symbols);
    }

    // Parse 2
    auto ast2 = state.parseExprFromString(nixCode, basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    std::map<std::string, Blake3Hash> hashes2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        auto * expr = def.e;
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind), expr, state.symbols);
    }

    ASSERT_EQ(hashes1.size(), hashes2.size());
    for (auto & [name, h1] : hashes1) {
        auto it = hashes2.find(name);
        ASSERT_NE(it, hashes2.end()) << "Missing binding: " << name;
        EXPECT_EQ(h1, it->second)
            << "Binding '" << name << "' hash differs between parses"
            << "\n  hash1=" << h1.toHex()
            << "\n  hash2=" << it->second.toHex();
    }
}

// Test with import path to check if path resolution affects hashes
TEST_F(NixBindingDeterminismTest, WithImportPath_SameScopeHash)
{
    std::string nixCode = R"(
        let
            lib = import ./lib;
        in {
            x = lib.id 1;
        }
    )";

    auto basePath = state.rootPath(CanonPath("/test.nix"));

    auto ast1 = state.parseExprFromString(nixCode, basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    auto ast2 = state.parseExprFromString(nixCode, basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash with import path should be deterministic"
        << "\n  hash1=" << scopeHash1.toHex()
        << "\n  hash2=" << scopeHash2.toHex();
}

// The real bug: parseExprFromFile vs parseExprFromString.
// Recording uses parseExprFromFile, verification uses parseExprFromString.
// This test creates a temp file and parses it both ways.
TEST_F(NixBindingDeterminismTest, ParseFile_vs_ParseString_SameBindingHash)
{
    std::string nixCode = "let\n  helper = x: x + 1;\nin {\n  foo = helper 42;\n  bar = \"hello\";\n}\n";

    TempTestFile file(nixCode);
    auto filePath = CanonPath(file.path.string());
    auto sourcePath = SourcePath(getFSSourceAccessor(), filePath);

    // Method 1: parseExprFromFile (recording path)
    auto ast1 = state.parseExprFromFile(sourcePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    std::map<std::string, Blake3Hash> hashes1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind),
            def.e, state.symbols);
    }

    // Method 2: parseExprFromString (verification path)
    auto contents = sourcePath.readFile();
    auto ast2 = state.parseExprFromString(
        std::move(contents), state.rootPath(filePath));
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash: parseExprFromFile vs parseExprFromString"
        << "\n  fromFile=" << scopeHash1.toHex()
        << "\n  fromString=" << scopeHash2.toHex();

    std::map<std::string, Blake3Hash> hashes2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind),
            def.e, state.symbols);
    }

    for (auto & [name, h1] : hashes1) {
        auto it = hashes2.find(name);
        ASSERT_NE(it, hashes2.end()) << "Missing binding: " << name;
        EXPECT_EQ(h1, it->second)
            << "Binding '" << name << "': parseExprFromFile vs parseExprFromString"
            << "\n  fromFile=" << h1.toHex()
            << "\n  fromString=" << it->second.toHex();
    }
}

// Pre-interning symbols in reverse order forces non-lexicographic Symbol IDs.
// Before the fix, formals->formals was sorted by Symbol ID, so pre-interning
// in reverse order would produce a different iteration order and different hash.
// After the fix, lexicographicOrder(symbols) sorts by string name regardless.
TEST_F(NixBindingDeterminismTest, FormalsOrder_DoesNotAffectScopeHash)
{
    std::string nixCode = "{ a, z }: { x = 1; }";
    auto basePath = state.rootPath(CanonPath("/test.nix"));

    // Parse without pre-interning — symbols assigned in parser encounter order
    auto ast1 = state.parseExprFromString(nixCode, basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    // Pre-intern "z" before "a" to force reverse Symbol ID ordering,
    // then parse again in a fresh EvalState that loads those symbols
    state.symbols.create("zzz_preintern_first");
    state.symbols.create("aaa_preintern_second");

    auto ast2 = state.parseExprFromString(nixCode, basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash must not depend on Symbol ID ordering"
        << "\n  hash1=" << scopeHash1.toHex()
        << "\n  hash2=" << scopeHash2.toHex();
}

// Different formals should produce different scope hashes (regression guard).
TEST_F(NixBindingDeterminismTest, DifferentFormals_DifferentScopeHash)
{
    auto basePath = state.rootPath(CanonPath("/test.nix"));

    auto ast1 = state.parseExprFromString("{ a, z }: { x = 1; }", basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    auto ast2 = state.parseExprFromString("{ a, z, extra }: { x = 1; }", basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_NE(scopeHash1, scopeHash2)
        << "Adding a formal should change the scope hash"
        << "\n  hash1=" << scopeHash1.toHex()
        << "\n  hash2=" << scopeHash2.toHex();
}

// Simulate cross-session symbol table differences: create a fresh EvalState
// with symbols pre-interned in reverse alphabetical order (as attr-vocab.sqlite
// load might do), parse the same code, and verify same scope hash.
TEST_F(NixBindingDeterminismTest, PreInternedSymbols_StableScopeHash)
{
    std::string nixCode = "{ config, lib, pkgs, ... }: { result = config; }";
    auto basePath = state.rootPath(CanonPath("/test.nix"));

    // Parse 1: normal order (symbols created by parser)
    auto ast1 = state.parseExprFromString(nixCode, basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    // Simulate attr-vocab.sqlite loading symbols in reverse alphabetical order
    // before parsing. This changes the Symbol IDs relative to parse 1.
    state.symbols.create("zzz_vocab_entry");
    state.symbols.create("pkgs_vocab_alias");
    state.symbols.create("lib_vocab_alias");
    state.symbols.create("config_vocab_alias");

    // Parse 2: same code but Symbol IDs for formals may differ
    auto ast2 = state.parseExprFromString(nixCode, basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2)
        << "Scope hash must be stable despite pre-interned symbols"
        << "\n  hash1=" << scopeHash1.toHex()
        << "\n  hash2=" << scopeHash2.toHex();
}

// ── Tests for the .parent() base-path fix (Bug 2) ──────────────────
//
// parseExprFromFile uses path.parent() as the base for resolving relative
// paths (eval.cc:3512). The NixBinding verification code in
// trace-verify-deps.cc must do the same when calling parseExprFromString.
// Before the fix, it passed the file path itself, causing relative imports
// like ./sub.nix or ../sibling.nix to resolve with an extra directory
// segment (e.g., dir/main.nix/sub.nix instead of dir/sub.nix).

// Helper: create a temp directory with a main.nix and a relative import target.
struct RelativeImportDir {
    std::filesystem::path dir;
    std::filesystem::path mainPath;

    RelativeImportDir(
        const std::string & mainContent,
        const std::string & relativePath,
        const std::string & targetContent)
    {
        auto base = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
        std::filesystem::create_directories(base);
        static int counter = 0;
        dir = base / ("relimport-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(dir);

        mainPath = dir / "main.nix";
        std::ofstream(mainPath) << mainContent;

        auto targetPath = dir / relativePath;
        std::filesystem::create_directories(targetPath.parent_path());
        std::ofstream(targetPath) << targetContent;
    }

    ~RelativeImportDir() { std::filesystem::remove_all(dir); }
    RelativeImportDir(const RelativeImportDir &) = delete;
    RelativeImportDir & operator=(const RelativeImportDir &) = delete;
};

// Positive: parseExprFromFile and parseExprFromString(content, parent())
// must produce identical binding hashes when the expression contains
// relative path imports. This is the exact code path used by recording
// (parseExprFromFile) vs verification (parseExprFromString with parent).
TEST_F(NixBindingDeterminismTest, RelativeImport_ParseFileVsParseStringParent_SameHash)
{
    RelativeImportDir setup(
        "{ x = import ./sub.nix; y = 42; }",
        "sub.nix",
        "{ val = 1; }");

    auto filePath = CanonPath(setup.mainPath.string());
    auto sourcePath = SourcePath(getFSSourceAccessor(), filePath);

    // Recording path: parseExprFromFile (uses path.parent() internally)
    auto ast1 = state.parseExprFromFile(sourcePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    std::map<std::string, Blake3Hash> hashes1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    // Verification path (FIXED): parseExprFromString with parent() as base
    auto contents = sourcePath.readFile();
    auto ast2 = state.parseExprFromString(
        std::move(contents), state.rootPath(filePath).parent());
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    EXPECT_EQ(scopeHash1, scopeHash2);

    std::map<std::string, Blake3Hash> hashes2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    for (auto & [name, h1] : hashes1) {
        auto it = hashes2.find(name);
        ASSERT_NE(it, hashes2.end()) << "Missing binding: " << name;
        EXPECT_EQ(h1, it->second)
            << "Binding '" << name << "': parseExprFromFile vs parseExprFromString(.parent())"
            << "\n  fromFile=" << h1.toHex()
            << "\n  fromString=" << it->second.toHex();
    }
}

// Positive: same test but with parent-relative import (../) to ensure
// multi-level relative paths are resolved correctly.
TEST_F(NixBindingDeterminismTest, ParentRelativeImport_ParseFileVsParseStringParent_SameHash)
{
    // Create:  dir/inner/main.nix  with  { x = import ../data.nix; }
    //          dir/data.nix
    auto base = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
    std::filesystem::create_directories(base);
    static int pcounter = 0;
    auto dir = base / ("parentrel-" + std::to_string(getpid()) + "-" + std::to_string(pcounter++));
    std::filesystem::create_directories(dir / "inner");
    auto mainPath = dir / "inner" / "main.nix";
    std::ofstream(mainPath) << "{ x = import ../data.nix; y = 1; }";
    std::ofstream(dir / "data.nix") << "42";

    auto filePath = CanonPath(mainPath.string());
    auto sourcePath = SourcePath(getFSSourceAccessor(), filePath);

    auto ast1 = state.parseExprFromFile(sourcePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    std::map<std::string, Blake3Hash> hashes1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    auto contents = sourcePath.readFile();
    auto ast2 = state.parseExprFromString(
        std::move(contents), state.rootPath(filePath).parent());
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    std::map<std::string, Blake3Hash> hashes2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    // The binding for "x" includes the resolved path in show() output.
    // With .parent(), both resolve ../data.nix to dir/data.nix.
    EXPECT_EQ(hashes1.at("x"), hashes2.at("x"))
        << "Binding 'x' with ../data.nix should match between parseExprFromFile and parseExprFromString(.parent())";

    std::filesystem::remove_all(dir);
}

// Negative: using the file path itself (old buggy behavior) as base for
// parseExprFromString produces DIFFERENT binding hashes than
// parseExprFromFile. This proves the .parent() fix is necessary.
TEST_F(NixBindingDeterminismTest, WrongBasePath_ProducesDifferentBindingHash)
{
    RelativeImportDir setup(
        "{ x = import ./sub.nix; y = 42; }",
        "sub.nix",
        "{ val = 1; }");

    auto filePath = CanonPath(setup.mainPath.string());
    auto sourcePath = SourcePath(getFSSourceAccessor(), filePath);

    // Recording path: parseExprFromFile (uses path.parent() internally)
    auto ast1 = state.parseExprFromFile(sourcePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    std::map<std::string, Blake3Hash> hashes1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes1[name] = computeNixBindingHash(
            scopeHash1, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    // WRONG base path: file path instead of parent directory (old bug).
    // ./sub.nix resolves to dir/main.nix/sub.nix instead of dir/sub.nix.
    auto contents = sourcePath.readFile();
    auto ast2 = state.parseExprFromString(
        std::move(contents), state.rootPath(filePath));  // NO .parent() — old bug
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    std::map<std::string, Blake3Hash> hashes2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        hashes2[name] = computeNixBindingHash(
            scopeHash2, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    // Binding "x" (import ./sub.nix) should DIFFER because the resolved
    // path differs: dir/sub.nix vs dir/main.nix/sub.nix.
    EXPECT_NE(hashes1.at("x"), hashes2.at("x"))
        << "Without .parent(), the relative import path should resolve differently"
        << "\n  fromFile=" << hashes1.at("x").toHex()
        << "\n  wrongBase=" << hashes2.at("x").toHex();

    // Binding "y" (= 42, no path) should still match — only path-containing
    // expressions are affected by the base path difference.
    EXPECT_EQ(hashes1.at("y"), hashes2.at("y"))
        << "Binding 'y' (literal value) should not be affected by base path";
}

// Negative: different relative import targets must produce different
// binding hashes (sanity check that the path IS part of the hash).
TEST_F(NixBindingDeterminismTest, DifferentRelativeImports_DifferentBindingHash)
{
    auto basePath = state.rootPath(CanonPath("/some/dir"));

    auto ast1 = state.parseExprFromString(
        "{ x = import ./alpha.nix; }", basePath);
    auto [attrs1, scope1] = findNonRecExprAttrs(ast1);
    ASSERT_NE(attrs1, nullptr);
    auto scopeHash1 = computeNixScopeHash(scope1, state.symbols);

    Blake3Hash hash1;
    for (auto & [sym, def] : *attrs1->attrs) {
        auto name = std::string(state.symbols[sym]);
        if (name == "x")
            hash1 = computeNixBindingHash(
                scopeHash1, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    auto ast2 = state.parseExprFromString(
        "{ x = import ./beta.nix; }", basePath);
    auto [attrs2, scope2] = findNonRecExprAttrs(ast2);
    ASSERT_NE(attrs2, nullptr);
    auto scopeHash2 = computeNixScopeHash(scope2, state.symbols);

    Blake3Hash hash2;
    for (auto & [sym, def] : *attrs2->attrs) {
        auto name = std::string(state.symbols[sym]);
        if (name == "x")
            hash2 = computeNixBindingHash(
                scopeHash2, name, static_cast<int>(def.kind), def.e, state.symbols);
    }

    EXPECT_NE(hash1, hash2)
        << "Different import targets should produce different binding hashes"
        << "\n  alpha=" << hash1.toHex()
        << "\n  beta=" << hash2.toHex();
}

} // namespace nix::eval_trace
