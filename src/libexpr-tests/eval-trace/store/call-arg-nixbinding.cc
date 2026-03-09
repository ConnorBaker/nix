#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"

#include <fstream>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── NixBinding coverage for call-argument attrsets ───────────────────
//
// aliases.nix pattern:
//   lib: self: super: let mapAliases = ...; in mapAliases { a = v1; b = v2; }
//
// The attrset is the argument to a function call. findNonRecExprAttrs
// rejects ExprCall, so NixBinding deps are never registered. The Content
// dep has no SC coverage → any file change causes a hard miss, even when
// the accessed binding didn't change.
//
// These tests contrast the eligible pattern (bare attrset body) with the
// call-arg pattern (ineligible) to demonstrate the gap.

struct NixBindingCoverageFixture : TraceStoreFixture
{
    static std::string readFile(const std::filesystem::path & path)
    {
        std::ifstream ifs(path);
        return {std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>()};
    }

    void rewriteFile(const std::filesystem::path & target,
                     const TempTestFile & source)
    {
        std::filesystem::copy_file(source.path, target,
            std::filesystem::copy_options::overwrite_existing);
        getFSSourceAccessor()->invalidateCache(CanonPath(target.string()));
        StatHashStore::instance().clear();
    }

    TempTestFile writeEligibleFile(
        const std::string & accessedValue,
        const std::string & unrelatedValue)
    {
        return TempTestFile(
            "lib:\n{\n"
            "  accessed = " + accessedValue + ";\n"
            "  unrelated = " + unrelatedValue + ";\n"
            "}\n");
    }

    TempTestFile writeCallArgFile(
        const std::string & accessedValue,
        const std::string & unrelatedValue)
    {
        return TempTestFile(
            "lib:\nlet\n"
            "  mapAliases = aliases: builtins.mapAttrs (n: v: v) aliases;\n"
            "in\nmapAliases {\n"
            "  accessed = " + accessedValue + ";\n"
            "  unrelated = " + unrelatedValue + ";\n"
            "}\n");
    }

    /// Parse a .nix file and compute the real NixBinding hash for a
    /// specific binding. Returns nullopt if findNonRecExprAttrs rejects
    /// the file (i.e., the call-arg pattern).
    std::optional<Blake3Hash> computeRealBindingHash(
        const std::filesystem::path & filePath,
        const std::string & bindingName)
    {
        auto content = readFile(filePath);
        auto expr = state.parseExprFromString(
            std::move(content),
            state.rootPath(CanonPath(filePath.string())).parent());

        auto [exprAttrs, scopeExprs] = findNonRecExprAttrs(expr);
        if (!exprAttrs) return std::nullopt;

        auto scopeHash = computeNixScopeHash(scopeExprs, state.symbols);
        for (auto & [sym, def] : *exprAttrs->attrs) {
            if (std::string(state.symbols[sym]) == bindingName) {
                return computeNixBindingHash(
                    scopeHash, bindingName,
                    static_cast<int>(def.kind), def.e, state.symbols);
            }
        }
        return std::nullopt;
    }

    /// Record a trace with Content + real NixBinding SC dep.
    /// Returns false if the file is ineligible (findNonRecExprAttrs fails).
    bool recordWithRealSCDep(
        TraceStore & db, AttrPathId pathId,
        const std::filesystem::path & filePath,
        const std::string & fileContent,
        const std::string & bindingName,
        const CachedResult & result)
    {
        auto bindingHash = computeRealBindingHash(filePath, bindingName);
        if (!bindingHash) return false;

        auto scKey = nlohmann::json{
            {"f", filePath.string()}, {"t", "n"},
            {"p", nlohmann::json::array({bindingName})}
        }.dump();

        db.record(pathId, result, {
            makeContentDep(pools(), filePath.string(), fileContent),
            {{DepType::StructuredContent,
              pools().intern<DepSourceId>(""),
              pools().intern<DepKeyId>(scKey)},
             *bindingHash},
        });
        return true;
    }
};

// ── Baseline: eligible pattern (bare attrset body) ──────────────────

TEST_F(NixBindingCoverageFixture, Eligible_UnrelatedChange_OverrideAccepts)
{
    auto file = writeEligibleFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok) << "Eligible file should produce NixBinding hash";

    auto v2file = writeEligibleFile("1", "999");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Eligible: unrelated change → SC override accepts → hit";
    assertCachedResultEquals(string_t{"result", {}}, result->value, state.symbols);
}

TEST_F(NixBindingCoverageFixture, Eligible_AccessedChange_OverrideRejects)
{
    auto file = writeEligibleFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok);

    auto v2file = writeEligibleFile("999", "2");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Eligible: accessed binding changed → SC dep fails → miss";
}

TEST_F(NixBindingCoverageFixture, Eligible_NoChange_DirectHit)
{
    auto file = writeEligibleFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok);

    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value())
        << "No change → Content dep passes → direct hit";
}

TEST_F(NixBindingCoverageFixture, Eligible_ScopeChange_OverrideRejects)
{
    auto file = TempTestFile(
        "{ lib }:\n{\n  accessed = lib.id 1;\n  unrelated = 2;\n}\n");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok);

    // Add a formal parameter → scope hash changes → all binding hashes change
    auto v2file = TempTestFile(
        "{ lib, stdenv }:\n{\n  accessed = lib.id 1;\n  unrelated = 2;\n}\n");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Scope change → all binding hashes change → miss";
}

// ── Call-arg pattern: findNonRecExprAttrs rejects ExprCall ───────────

TEST_F(NixBindingCoverageFixture, CallArg_FindNonRecExprAttrs_ReturnsHash)
{
    // findNonRecExprAttrs walks into ExprCall arguments to find the attrset.
    auto file = writeCallArgFile("1", "2");
    auto hash = computeRealBindingHash(file.path, "accessed");
    ASSERT_TRUE(hash.has_value())
        << "findNonRecExprAttrs should walk into ExprCall arg";

    // The hash should differ from the eligible pattern (different scope:
    // call-arg includes the call expression in the scope hash).
    auto eligible = writeEligibleFile("1", "2");
    auto eligibleHash = computeRealBindingHash(eligible.path, "accessed");
    ASSERT_TRUE(eligibleHash.has_value());
    EXPECT_NE(*hash, *eligibleHash)
        << "Call-arg and eligible should have different scope hashes "
           "(call expression is part of the scope)";
}

TEST_F(NixBindingCoverageFixture, CallArg_UnrelatedChange_OverrideAccepts)
{
    // With findNonRecExprAttrs walking into ExprCall, the call-arg pattern
    // gets SC coverage. Changing an unrelated binding → SC dep on "accessed"
    // passes → override accepts → hit. Same behavior as the eligible case.
    auto file = writeCallArgFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok) << "Call-arg file should now produce NixBinding hash";

    auto v2file = writeCallArgFile("1", "999");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Call-arg: unrelated change → SC override accepts → hit";
    assertCachedResultEquals(string_t{"result", {}}, result->value, state.symbols);
}

TEST_F(NixBindingCoverageFixture, CallArg_AccessedChange_OverrideRejects)
{
    // Changing the accessed binding → SC dep fails → override rejects → miss.
    // Now the miss is for the RIGHT reason (SC dep detects the change).
    auto file = writeCallArgFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok);

    auto v2file = writeCallArgFile("999", "2");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Call-arg: accessed binding changed → SC dep fails → miss";
}

TEST_F(NixBindingCoverageFixture, CallArg_CalledFunctionChange_OverrideRejects)
{
    // Changing the called function (e.g., mapAliases → mapAliases2) changes
    // the scope hash → all binding hashes change → SC deps fail → miss.
    auto file = TempTestFile(
        "lib:\nlet\n"
        "  mapAliases = aliases: builtins.mapAttrs (n: v: v) aliases;\n"
        "in\nmapAliases {\n  accessed = 1;\n  unrelated = 2;\n}\n");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    bool ok = recordWithRealSCDep(db, rootPath(), file.path, v1,
                                  "accessed", string_t{"result", {}});
    ASSERT_TRUE(ok);

    // Change the called function name
    auto v2file = TempTestFile(
        "lib:\nlet\n"
        "  mapAliases = aliases: builtins.mapAttrs (n: v: v) aliases;\n"
        "in\nbuiltins.mapAttrs (n: v: v) {\n  accessed = 1;\n  unrelated = 2;\n}\n");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Changing the called function → scope hash changes → miss";
}

} // namespace nix::eval_trace
