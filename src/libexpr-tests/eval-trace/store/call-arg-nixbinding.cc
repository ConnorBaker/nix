#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"

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
        }, true);
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

TEST_F(NixBindingCoverageFixture, CallArg_FindNonRecExprAttrs_ReturnsNull)
{
    // Directly verify that findNonRecExprAttrs rejects the call-arg pattern.
    auto file = writeCallArgFile("1", "2");
    auto hash = computeRealBindingHash(file.path, "accessed");
    EXPECT_FALSE(hash.has_value())
        << "findNonRecExprAttrs should return nullptr for ExprCall body";
}

TEST_F(NixBindingCoverageFixture, CallArg_FindNonRecExprAttrs_EligibleReturnsHash)
{
    // Contrast: findNonRecExprAttrs accepts the eligible pattern.
    auto file = writeEligibleFile("1", "2");
    auto hash = computeRealBindingHash(file.path, "accessed");
    EXPECT_TRUE(hash.has_value())
        << "findNonRecExprAttrs should return the attrset for bare body";
}

TEST_F(NixBindingCoverageFixture, CallArg_UnrelatedChange_MissesWithoutSC)
{
    // The call-arg pattern has NO SC coverage. Changing an unrelated
    // binding → Content fails, no override → miss.
    // This SHOULD be a hit (like Eligible_UnrelatedChange_OverrideAccepts)
    // but ISN'T because findNonRecExprAttrs rejects ExprCall.
    auto file = writeCallArgFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    db.record(rootPath(), string_t{"result", {}}, {
        makeContentDep(pools(), file.path.string(), v1),
    }, true);

    auto v2file = writeCallArgFile("1", "999");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Call-arg: misses due to lack of SC coverage. "
           "Fix: extend findNonRecExprAttrs to walk into ExprCall args.";
}

TEST_F(NixBindingCoverageFixture, CallArg_AccessedChange_MissesForWrongReason)
{
    // Miss is correct in outcome but wrong in mechanism: there's no SC dep
    // to detect the change. Miss comes from bare Content dep failure.
    auto file = writeCallArgFile("1", "2");
    auto v1 = readFile(file.path);

    auto db = makeDb();
    db.record(rootPath(), string_t{"result", {}}, {
        makeContentDep(pools(), file.path.string(), v1),
    }, true);

    auto v2file = writeCallArgFile("999", "2");
    rewriteFile(file.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Correct miss, wrong reason: no SC dep to detect binding change.";
}

TEST_F(NixBindingCoverageFixture, CallArg_ManualSCDep_VerificationCannotRecompute)
{
    // Even if we manually record an SC dep with the correct hash, verification
    // re-parses the file and calls findNonRecExprAttrs to recompute.
    // Since findNonRecExprAttrs rejects ExprCall → empty hash map →
    // computeCurrentHash returns nullopt → SC dep fails → no override.
    // Both recording AND verification paths need the fix.
    auto eligible = writeEligibleFile("1", "2");
    auto callarg = writeCallArgFile("1", "2");

    // Compute the hash from the ELIGIBLE version (same binding AST)
    auto bindingHash = computeRealBindingHash(eligible.path, "accessed");
    ASSERT_TRUE(bindingHash.has_value());

    // Record against the CALL-ARG file with the eligible file's hash
    auto v1 = readFile(callarg.path);
    auto db = makeDb();
    auto scKey = nlohmann::json{
        {"f", callarg.path.string()}, {"t", "n"},
        {"p", nlohmann::json::array({"accessed"})}
    }.dump();
    db.record(rootPath(), string_t{"result", {}}, {
        makeContentDep(pools(), callarg.path.string(), v1),
        {{DepType::StructuredContent,
          pools().intern<DepSourceId>(""),
          pools().intern<DepKeyId>(scKey)},
         *bindingHash},
    }, true);

    // Change only unrelated binding
    auto v2file = writeCallArgFile("1", "999");
    rewriteFile(callarg.path, v2file);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "SC dep exists but verification can't recompute: "
           "findNonRecExprAttrs rejects ExprCall in the verification path.";
}

} // namespace nix::eval_trace
